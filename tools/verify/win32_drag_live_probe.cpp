/* Win32 OLE 拖入 / 拖出桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖接缝：
//   拖入：`RegisterDragDrop` + `IDropTarget` 回调 → `detail::Win32DragBridge` → 折算层
//   （`detail/system_drag_translate`）→ 拖入入口（`event/detail/system_drag_ingress`）→
//   `EventDispatcher` → 控件 `query_system_drag_accept` / `on_system_drop`，以及 `allowed`
//   回写 `DROPEFFECT` 的光标反馈闭环。
//   拖出：控件 `begin_system_drag()` → `system_drag::begin` → 拖出后端（平台桥经
//   `detail::register_system_drag_out_backend` 登记）→ 库自管拖拽线程上的 `DoDragDrop`
//   + `IDropSource` + 拖出数据对象；终态经 `complete_drag_out_session` 登记、帧循环
//   `pump_reads()` 投递 `on_system_drag_end`。
//   后端：`Win32Surface`（GDI）与 `D3D11Surface`（GPU）共用同一 `Win32Host` 宿主与同一份
//   桥实例，故本探针只跑一路（GDI），另一路无需重复验收。
//
// 为什么必须真机：无头 CI 里没有真实窗口，`RegisterDragDrop` 不会成功、OS 会话回调永不到达，
// 而「跨进程会话」这件事本身不存在——裸 `Window` 也没有 handler，只有 `Application` 登记的
// `WindowHost` 才把后端事件接进 `EventDispatcher`。单元测试覆盖的是平台中立部分（折算层
// `utest_system_drag_translate`、拖入入口 `utest_system_drag_ingress`）；本探针覆盖的是
// 「桥与 Windows OLE 的接缝」这一条无头证明不了的部分。
//
// 自动段（无需人工拖文件）逐项验收：
//   ① 能力位：本机构建下 `platform().capabilities().system_drag_in` 为 true（Win32 / D3D11 同翻）。
//   ② 注册：对同一 HWND 再次 `RegisterDragDrop` 必须返回 `DRAGDROP_E_ALREADYREGISTERED`
//      —— 这是「宿主确实注册了 OLE 拖入目标」的反向证明；若返回 S_OK 则说明没注册（FAIL 并撤销）。
//   ③ 派发链：经 `EventDispatcher` 投一条合成 Enter 事件，accept 查询发生且 `allowed` 为
//      proposed 与 accept 的求交结果（真机构建下派发语义与无头单测一致）。
//   ④ 能力位：`platform().capabilities().system_drag_out` 为 true（拖出同批接线）。
//   ⑤ 自拖放全链路（无人值守）：光标泊在落放面板上且不按任何鼠标按键，
//      `begin_system_drag()` 发起会话 → 拖拽线程 `DoDragDrop` → `QueryContinueDrag` 一见
//      「左键未按」即返回落放 → OLE 把落点投回本窗口已注册的 `IDropTarget` → accept 查询
//      谈成 `DROPEFFECT_COPY` → 终态经帧循环投递 `on_system_drag_end(Succeeded, Copy)`，
//      且落放载荷与 `provide` 给出的文本一致（要约 → 数据对象 → 拖入侧载荷往返）。
//      抖动线程（±1px / 15ms）只为驱动 `DoDragDrop` 的输入驱动型模态循环，不改变落点。
//
// 人工段（`--interactive`）：窗口常驻，逐帧打印拖入四态与拖出三态。
//   拖入：从资源管理器拖一个或多个文件进来，逐项肉眼核对：① 进入窗口即有 Enter、移动时
//   持续 Over、移出有 Leave、放下有 Drop；② 悬停时 OS 光标形状随 accept 结果变化（可复制 /
//   禁止放置）；③ Drop 后打印的文件路径与拖入的文件一致（uri-list 解码往返）；④ 按住
//   Ctrl / Shift 时 proposed 随之变化。
//   拖出：从下方拖源面板按住左键拖到本窗外（记事本 / 资源管理器均可）后松开，期望
//   begin → end(Succeeded, Copy)；拖动中按 Esc，期望 begin → cancel → end(Cancelled, None)。
//   Ctrl / Shift 修饰会改变 OS 提议的效果（本探针要约未提供 Move 确认，效果恒在 Copy | Link 内）。
//
// 构建：
//   cmake -S . -B build-verify-drag -G Ninja -DAURORA_BACKEND_WIN32=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify-drag --target aurora_verify_win32_drag
//   build-verify-drag\aurora_verify_win32_drag.exe [--interactive]
//
// 退出码：
//   0  自动段全部通过
//   2  环境不可用（建窗 / HWND 取不到）
//   4  部分验收项不符 —— 见逐行 PASS/FAIL
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_drag can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/mime_types.h"
#include "aurora/app/scene.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/node.h"
#include "aurora/window/platform.h"
#include "aurora/window/window.h"
#include "verify_args.h"
#include "verify_print.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 逐项判据：打印 PASS/FAIL 并累加失败数。
/// @param ok 判据结果。
/// @param label 判据名。
/// @param failures 失败计数（由 `main` 持有、显式传入，不留命名空间级可变状态）。
auto check(bool ok, const std::string &label, int &failures) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        ++failures;
    }
}

/// @brief 落放面板：记录拖入四态与最近一次的格式 / 动作 / 文件 / 文本；自拖放自动段里
/// 同时充当落点——光标泊在本面板上，OLE 把落点投回本窗口已注册的 `IDropTarget`。
class DropPanel final : public aurora::LeafWidget {
  public:
    std::vector<std::string> phase_log;  ///< 收到的阶段名（按序）
    std::vector<std::string> last_formats;  ///< 最近一次事件的格式清单
    std::vector<std::string> last_files;  ///< 最近一次落放的文件路径
    std::string last_text;  ///< 最近一次落放的纯文本（text/plain 解码；自拖放往返判据）
    aurora::DragAction last_allowed = aurora::DragAction::None;  ///< 派发器写定的允许动作
    aurora::DragAction accept = aurora::DragAction::Copy;  ///< `query_system_drag_accept` 的返回值
    int accept_queries = 0;  ///< accept 查询次数

    auto type_name() const -> const char * override { return "DropPanel"; }

    auto on_layout(const aurora::Constraints &c, [[maybe_unused]] const aurora::BuildContext &ctx)
        -> aurora::Size override {
        size_ = c.constrain(aurora::Size{.width = 480.0F, .height = 200.0F});
        return size_;
    }

    auto on_paint([[maybe_unused]] aurora::Painter &p, [[maybe_unused]] const aurora::Rect &bounds,
                  [[maybe_unused]] const aurora::BuildContext &ctx) -> void override {}

    auto on_system_drop(aurora::SystemDropEvent &e) -> void override {
        switch (e.phase) {
            case aurora::SystemDropPhase::Enter:
                phase_log.emplace_back("Enter");
                break;
            case aurora::SystemDropPhase::Over:
                phase_log.emplace_back("Over");
                break;
            case aurora::SystemDropPhase::Leave:
                phase_log.emplace_back("Leave");
                break;
            case aurora::SystemDropPhase::Drop:
                phase_log.emplace_back("Drop");
                break;
        }
        last_formats = e.formats;
        last_allowed = e.allowed;
        if (e.phase == aurora::SystemDropPhase::Drop && e.payload != nullptr) {
            const aurora::Result<std::vector<std::string>> files = e.files();
            if (files) {
                last_files = files.value();
            }
            const aurora::Result<std::string> text = e.text();
            if (text) {
                last_text = text.value();
            }
        }
        e.is_handled = true;
    }

    [[nodiscard]] auto query_system_drag_accept([[maybe_unused]] const std::vector<std::string> &formats)
        -> aurora::DragAction override {
        ++accept_queries;
        return accept;
    }
};

// 探针布局（固定窗口 480x300）：上 480x200 落放面板 + 下 480x100 拖源面板；自拖放泊点取
// 落放面板中心（客户区 240,100），与两块面板 on_layout 的请求尺寸对应。
constexpr int kProbeParkX = 240;
constexpr int kProbeParkY = 100;

// 自拖放要约给出的文本：自动段断言「落放载荷与 provide 给出的文本一致」（要约 → 数据对象
// → 拖入侧载荷的往返）。
constexpr const char kDragOutProbeText[] = "aurora win32 drag-out self-drop payload";

// 自拖放终态的墙钟截止：正常路径远小于 1s（发起即返回、无按键时 QueryContinueDrag 立即
// 落放）；20s 是给慢机 / 远程桌面的余量，超时即判 FAIL 并打印，不悬挂。
constexpr auto kDragOutDeadline = std::chrono::seconds{20};

/// @brief 终态结局 → 可读名（探针输出用；库侧无字符串化助手）。
[[nodiscard]] auto outcome_name(aurora::SystemDragOutcome outcome) -> const char * {
    switch (outcome) {
        case aurora::SystemDragOutcome::Succeeded:
            return "Succeeded";
        case aurora::SystemDragOutcome::Cancelled:
            return "Cancelled";
        case aurora::SystemDragOutcome::Rejected:
            return "Rejected";
        case aurora::SystemDragOutcome::Failed:
            return "Failed";
    }
    return "?";
}

/// @brief 拖放动作 → 可读名（探针输出用；库侧无字符串化助手）。
[[nodiscard]] auto action_name(aurora::DragAction action) -> const char * {
    switch (action) {
        case aurora::DragAction::None:
            return "None";
        case aurora::DragAction::Copy:
            return "Copy";
        case aurora::DragAction::Move:
            return "Move";
        case aurora::DragAction::Link:
            return "Link";
    }
    return "?";
}

/// @brief 拖源面板：声明一份 text/plain 要约，记录拖出三态（begin / cancel / end）。
///
/// 人工段里用户从本面板按住左键拖出；自动段由探针直接调 `begin_system_drag()` 发起
/// 自拖放（不经过任何指针事件，也不需要面板获得焦点）。
class DragSourcePanel final : public aurora::LeafWidget {
  public:
    std::vector<std::string> out_log;  ///< 拖出态序列（begin / cancel / end:<outcome>:<action>）
    bool begin_fired = false;  ///< `on_system_drag_begin` 是否派发
    bool end_seen = false;  ///< 终态是否经帧循环投递
    aurora::SystemDragOutcome end_outcome = aurora::SystemDragOutcome::Failed;  ///< 最近终态结局
    aurora::DragAction end_action = aurora::DragAction::None;  ///< 最近终态动作

    auto type_name() const -> const char * override { return "DragSourcePanel"; }

    auto on_layout(const aurora::Constraints &c, [[maybe_unused]] const aurora::BuildContext &ctx)
        -> aurora::Size override {
        size_ = c.constrain(aurora::Size{.width = 480.0F, .height = 100.0F});
        return size_;
    }

    auto on_paint([[maybe_unused]] aurora::Painter &p, [[maybe_unused]] const aurora::Rect &bounds,
                  [[maybe_unused]] const aurora::BuildContext &ctx) -> void override {}

    [[nodiscard]] auto on_provide_drag() -> std::optional<aurora::DragOffer> override {
        aurora::DragOffer offer;
        offer.formats.emplace_back(aurora::AURORA_MIME_TEXT_PLAIN);
        offer.default_action = aurora::DragAction::Copy;
        offer.provide = [](const std::string &mime) -> aurora::Result<std::vector<std::uint8_t>> {
            if (mime != aurora::AURORA_MIME_TEXT_PLAIN) {
                return make_error(aurora::ErrorCode::SystemDragReadFailed,
                                  aurora::ErrorParams{{"mime", mime}, {"detail", "probe offers text/plain only"}});
            }
            const std::string text = kDragOutProbeText;
            return std::vector<std::uint8_t>(text.begin(), text.end());
        };
        return offer;
    }

    auto on_system_drag_begin() -> void override {
        begin_fired = true;
        out_log.emplace_back("begin");
    }

    auto on_system_drag_cancel() -> void override { out_log.emplace_back("cancel"); }

    auto on_system_drag_end(aurora::SystemDragOutcome outcome, aurora::DragAction action) -> void override {
        end_seen = true;
        end_outcome = outcome;
        end_action = action;
        out_log.emplace_back(std::string{"end:"} + outcome_name(outcome) + ":" + action_name(action));
    }
};

/// @brief 光标抖动线程：`DoDragDrop` 的模态循环由输入驱动——完全静止的光标可能收不到
/// `QueryContinueDrag` 回调（Win32 F16 同进程自拖放实测同款手法：±1px / 20ms 抖动保证
/// 循环推进）。本探针不按任何鼠标按键，`QueryContinueDrag` 一见「左键未按」即返回落放，
/// 故抖动只影响循环节拍、不影响落点（始终在泊点 ±1px 内）。
auto jiggle_cursor(std::atomic<bool> *run) -> void {
    POINT base{};
    GetCursorPos(&base);
    int step = 0;
    while (run->load(std::memory_order_relaxed)) {
        POINT moved = base;
        moved.x += (step % 2 == 0) ? 1 : -1;
        SetCursorPos(moved.x, moved.y);
        ++step;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

/// @brief 自拖放自动段的断言收尾（终态经帧循环投递后调用）。
auto settle_drag_out_leg(DragSourcePanel &source, DropPanel &panel, const int accept_queries_before, int &failures)
    -> void {
    check(source.begin_fired, "on_system_drag_begin dispatched on session start", failures);
    check(source.end_seen, "on_system_drag_end delivered through the frame loop (pump_reads)", failures);
    if (source.end_seen) {
        check(source.end_outcome == aurora::SystemDragOutcome::Succeeded,
              std::string{"terminal outcome is Succeeded (got "} + outcome_name(source.end_outcome) + ")", failures);
        check(source.end_action == aurora::DragAction::Copy,
              std::string{"negotiated action is Copy (got "} + action_name(source.end_action) + ")", failures);
    }
    check(panel.accept_queries > accept_queries_before, "accept query happened for the self-drop", failures);
    check(std::find(panel.phase_log.begin(), panel.phase_log.end(), "Drop") != panel.phase_log.end(),
          "self-drop reached this window's drop target (Drop phase)", failures);
    check(panel.last_text == kDragOutProbeText,
          std::string{"drop payload round-trips the offered text/plain (got \""} + panel.last_text + "\")", failures);
}

/// @brief 人工段：逐帧打印新增的拖出态（begin / cancel / end）。
auto report_drag_out(DragSourcePanel &source, std::size_t &printed) -> void {
    if (source.out_log.size() > printed) {
        for (std::size_t i = printed; i < source.out_log.size(); ++i) {
            emit(std::string("[drag-out] ") + source.out_log[i]);
        }
        printed = source.out_log.size();
    }
}

/// @brief 自动段判据（真机环境 + 已建窗口）。
auto run_automated(HWND hwnd, const aurora::Platform &plat, DropPanel &panel, int &failures) -> void {
    // ① 能力位：Win32 / D3D11 两类 Surface 共用同一宿主与桥，均应为 true。
    const bool in_supported = plat.surface == aurora::SurfaceKind::Win32 || plat.surface == aurora::SurfaceKind::D3D11;
    check(in_supported, "surface kind is Win32 or D3D11 (OLE drag-in backend family)", failures);
    check(plat.capabilities().system_drag_in, "capabilities().system_drag_in is true after wiring", failures);

    // ② 注册：再次 `RegisterDragDrop` 同一 HWND 必须被拒（ALREADYREGISTERED = 宿主已注册）。
    //    返回 S_OK 意味着本窗口此前没有注册 —— 桥未接线，该项 FAIL 并撤销这次误注册。
    struct NoopTarget final : IDropTarget {
        auto STDMETHODCALLTYPE QueryInterface([[maybe_unused]] REFIID riid, void **object) -> HRESULT override {
            if (object != nullptr) {
                *object = nullptr;
            }
            return E_NOINTERFACE;
        }
        auto STDMETHODCALLTYPE AddRef() -> ULONG override { return 1; }
        auto STDMETHODCALLTYPE Release() -> ULONG override { return 1; }
        auto STDMETHODCALLTYPE DragEnter([[maybe_unused]] IDataObject *data, [[maybe_unused]] DWORD key_state,
                                         [[maybe_unused]] POINTL pt, [[maybe_unused]] DWORD *effect)
            -> HRESULT override {
            return S_OK;
        }
        auto STDMETHODCALLTYPE DragOver([[maybe_unused]] DWORD key_state, [[maybe_unused]] POINTL pt,
                                        [[maybe_unused]] DWORD *effect) -> HRESULT override {
            return S_OK;
        }
        auto STDMETHODCALLTYPE DragLeave() -> HRESULT override { return S_OK; }
        auto STDMETHODCALLTYPE Drop([[maybe_unused]] IDataObject *data, [[maybe_unused]] DWORD key_state,
                                    [[maybe_unused]] POINTL pt, [[maybe_unused]] DWORD *effect) -> HRESULT override {
            return S_OK;
        }
    } probe_target;
    const HRESULT again = RegisterDragDrop(hwnd, &probe_target);
    const bool already = (again == DRAGDROP_E_ALREADYREGISTERED);
    check(already, "RegisterDragDrop on the same HWND reports DRAGDROP_E_ALREADYREGISTERED (bridge registered)",
          failures);
    if (!already && SUCCEEDED(again)) {
        RevokeDragDrop(hwnd);  // 误注册：撤销，避免污染后续人工段
    }

    // ③ 派发链：合成一条 Enter 事件经真派发器，accept 查询发生且 allowed 为求交结果。
    aurora::SystemDropEvent event;
    event.phase = aurora::SystemDropPhase::Enter;
    event.position = aurora::Point{.x = 10.0F, .y = 10.0F};
    event.formats.emplace_back("text/uri-list");
    event.proposed = aurora::DragAction::Move;
    panel.accept = aurora::DragAction::Move;
    const bool dispatched = aurora::EventDispatcher::dispatch(panel, event);
    check(dispatched, "synthetic Enter event reaches the hit target through the real dispatcher", failures);
    check(panel.accept_queries > 0, "accept query happens on Enter (cursor feedback has a source)", failures);
    check(event.allowed == aurora::DragAction::Move, "allowed is the intersection of proposed and accepted", failures);

    // ④ 能力位：拖出同批接线（Win32 / D3D11 两类 Surface 均应为 true；未接线时 begin 恒
    //    返回 not-supported，⑤ 也就无从谈起）。
    check(plat.capabilities().system_drag_out, "capabilities().system_drag_out is true after wiring", failures);
}

/// @brief 人工段：逐帧打印新增的四态与最近一次落放内容。
auto report_interactive(DropPanel &panel, std::size_t &printed, int &last_files_count) -> void {
    if (panel.phase_log.size() > printed) {
        for (std::size_t i = printed; i < panel.phase_log.size(); ++i) {
            emit(std::string("[drag] phase=") + panel.phase_log[i] +
                 " formats=" + std::to_string(panel.last_formats.size()));
        }
        printed = panel.phase_log.size();
    }
    const int files = static_cast<int>(panel.last_files.size());
    if (files != last_files_count) {
        last_files_count = files;
        emit(std::string("[drag] drop files=") + std::to_string(files));
        for (const std::string &path : panel.last_files) {
            emit(std::string("[drag]   ") + path);
        }
    }
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码呈现（捕获反而把它压成 0），与 demo 入口同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 OLE drag-in / drag-out bridge live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    emit("==== Win32 OLE drag bridge live acceptance (drag-in + drag-out) ====");

    auto panel = std::make_shared<DropPanel>();
    auto source = std::make_shared<DragSourcePanel>();
    panel->layout(aurora::Constraints{}, aurora::BuildContext{});
    source->layout(aurora::Constraints{}, aurora::BuildContext{});
    auto made = aurora::create_native_window(
        aurora::WindowOptions{.size = aurora::Size{.width = 480.0F, .height = 300.0F}, .title = "Aurora drag verify"});
    if (!made) {
        AURORA_LOG_ERROR("verify", "cannot create native window");
        return 2;
    }
    aurora::Window *window = made.value().get();
    auto *hwnd = static_cast<HWND>(window->surface().native_handle());
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("verify", "cannot obtain HWND");
        return 2;
    }
    const aurora::Platform plat = aurora::platform();
    // 两种模式都不设帧预算：自动段由帧回调在终态到达 / 截止时自行 quit。power_saving 关掉是
    // 为了保证自拖放期间帧循环持续泵平台消息——OS 会话回调经 COM 消息到达，空闲阻塞等待可能
    // 把落放回调的派发推迟到截止之后。
    aurora::Application app(aurora::Scene{aurora::Node{aurora::Column(
                                aurora::ColumnProps{.children = {aurora::Node{panel}, aurora::Node{source}}})}},
                            std::move(made.value()),
                            aurora::WindowOptions{.size = aurora::Size{.width = 480.0F, .height = 300.0F},
                                                  .title = "Aurora drag verify",
                                                  .max_frames = -1,
                                                  .power_saving = false});
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);

    int frame = 0;
    int failures = 0;
    std::size_t printed = 0;
    int last_files_count = 0;
    std::size_t printed_out = 0;
    bool drag_out_armed = false;
    bool drag_out_settled = false;
    int accept_queries_before = 0;
    std::chrono::steady_clock::time_point drag_out_began{};
    std::atomic<bool> jiggle_run{false};
    std::thread jiggle;
    app.set_on_frame([&]() -> void {
        ++frame;
        if (frame == 3) {
            run_automated(hwnd, plat, *panel, failures);
            if (!interactive) {
                emit("");
                emit("---- Automated drag-out leg: self drag-out onto this window ----");
                emit("Parking the cursor over the drop panel and starting a session with no mouse");
                emit("button held: QueryContinueDrag reports \"left button released\" at once, so the");
                emit("OS drag loop drops onto this very window without any human input.");
            } else {
                emit("");
                emit("---- Manual section ----");
                emit("Drag-in: drag one or more files from Explorer into the upper panel.");
                emit("Expect: Enter on entry -> repeated Over while moving -> Leave on exit -> Drop on");
                emit("        release; the cursor shape follows the accept verdict; dropped paths are");
                emit("        listed below. Hold Ctrl / Shift while dragging to see the proposed change.");
                emit("Drag-out: press and hold the left button on the lower panel, drag outside this");
                emit("        window (Notepad / Explorer), then release: expect begin -> end(Succeeded,");
                emit("        Copy). Press Esc mid-drag: expect begin -> cancel -> end(Cancelled, None).");
                emit("(no automated verdict in this section; closing the window exits)");
            }
        }
        if (!interactive && frame >= 5 && !drag_out_armed) {
            // ⑤ 自拖放全链路（无人值守）：泊光标 → 抖动驱动拖拽循环 → 控件层发起 → 帧内断言终态。
            drag_out_armed = true;
            panel->accept = aurora::DragAction::Copy;  // 判据 ③ 借去过 Move，落放腿改回 Copy
            accept_queries_before = panel->accept_queries;
            POINT park{.x = kProbeParkX, .y = kProbeParkY};
            ClientToScreen(hwnd, &park);
            SetCursorPos(park.x, park.y);
            jiggle_run.store(true, std::memory_order_relaxed);
            jiggle = std::thread(jiggle_cursor, &jiggle_run);
            const auto started = source->begin_system_drag();
            check(static_cast<bool>(started), "begin_system_drag() starts the session (Ok)", failures);
            drag_out_began = std::chrono::steady_clock::now();
        }
        if (!interactive && drag_out_armed && !drag_out_settled) {
            const auto elapsed = std::chrono::steady_clock::now() - drag_out_began;
            if (source->end_seen) {
                settle_drag_out_leg(*source, *panel, accept_queries_before, failures);
                emit("[info] self drag-out settled in " +
                     aurora_verify::format_int(static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count())) +
                     " ms");
                drag_out_settled = true;
                jiggle_run.store(false, std::memory_order_relaxed);
                app.quit();
            } else if (elapsed > kDragOutDeadline) {
                check(false, "self drag-out terminal state delivered within the deadline", failures);
                emit("[info] drag-out leg timed out with no terminal state (OS drag loop never settled?)");
                drag_out_settled = true;
                jiggle_run.store(false, std::memory_order_relaxed);
                app.quit();
            }
        }
        if (interactive) {
            report_interactive(*panel, printed, last_files_count);
            report_drag_out(*source, printed_out);
        }
    });
    app.run();
    jiggle_run.store(false, std::memory_order_relaxed);
    if (jiggle.joinable()) {
        jiggle.join();
    }

    if (!interactive) {
        emit("");
        emit("Automated section done. To verify against real OS drag sessions, rerun with --interactive.");
    }
    return failures == 0 ? 0 : 4;
}
