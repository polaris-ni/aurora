/* Win32 OLE 拖入桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖接缝：`RegisterDragDrop` + `IDropTarget` 回调 → `detail::Win32DragBridge` → 折算层
// （`detail/system_drag_translate`）→ 拖入入口（`event/detail/system_drag_ingress`）→
// `EventDispatcher` → 控件 `query_system_drag_accept` / `on_system_drop`，以及 `allowed` 回写
// `DROPEFFECT` 的光标反馈闭环。后端：`Win32Surface`（GDI）与 `D3D11Surface`（GPU）共用同一
// `Win32Host` 宿主与同一份桥实例，故本探针只跑一路（GDI），另一路无需重复验收。
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
//
// 人工段（`--interactive`）：窗口常驻，逐帧打印拖入四态。从资源管理器拖一个或多个文件进来，
// 逐项肉眼核对：① 进入窗口即有 Enter、移动时持续 Over、移出有 Leave、放下有 Drop；
// ② 悬停时 OS 光标形状随 accept 结果变化（可复制 / 禁止放置）；③ Drop 后打印的文件路径与
// 拖入的文件一致（uri-list 解码往返）；④ 按住 Ctrl / Shift 时 proposed 随之变化。
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

#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/core/types.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
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

/// @brief 拖入面板：记录四态序列与最近一次的格式 / 动作 / 文件清单。
class DropPanel final : public aurora::LeafWidget {
  public:
    std::vector<std::string> phase_log;  ///< 收到的阶段名（按序）
    std::vector<std::string> last_formats;  ///< 最近一次事件的格式清单
    std::vector<std::string> last_files;  ///< 最近一次落放的文件路径
    aurora::DragAction last_allowed = aurora::DragAction::None;  ///< 派发器写定的允许动作
    aurora::DragAction accept = aurora::DragAction::Copy;  ///< `query_system_drag_accept` 的返回值
    int accept_queries = 0;  ///< accept 查询次数

    auto type_name() const -> const char * override { return "DropPanel"; }

    auto on_layout(const aurora::Constraints &c, [[maybe_unused]] const aurora::BuildContext &ctx)
        -> aurora::Size override {
        size_ = c.constrain(aurora::Size{.width = 480.0F, .height = 300.0F});
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
        }
        e.is_handled = true;
    }

    [[nodiscard]] auto query_system_drag_accept([[maybe_unused]] const std::vector<std::string> &formats)
        -> aurora::DragAction override {
        ++accept_queries;
        return accept;
    }
};

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
    const auto cli = aurora_verify::parse_interactive("Win32 OLE drag-in bridge live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    emit("==== Win32 OLE drag-in bridge live acceptance ====");

    auto panel = std::make_shared<DropPanel>();
    panel->layout(aurora::Constraints{}, aurora::BuildContext{});
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
    aurora::Application app(aurora::Scene{aurora::Node{panel}}, std::move(made.value()),
                            aurora::WindowOptions{.size = aurora::Size{.width = 480.0F, .height = 300.0F},
                                                  .title = "Aurora drag verify",
                                                  .max_frames = interactive ? -1 : 60});
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);

    int frame = 0;
    int failures = 0;
    std::size_t printed = 0;
    int last_files_count = 0;
    app.set_on_frame([&]() -> void {
        ++frame;
        if (frame == 3) {
            run_automated(hwnd, plat, *panel, failures);
            if (!interactive) {
                app.quit();
            } else {
                emit("");
                emit("---- Manual section: drag one or more files from Explorer into this window ----");
                emit("Expect: Enter on entry -> repeated Over while moving -> Leave on exit -> Drop on release;");
                emit("        the cursor shape follows the accept verdict; dropped paths are listed below.");
                emit("Hold Ctrl / Shift while dragging to see the proposed action change.");
                emit("(no automated verdict in this section; closing the window exits)");
            }
        }
        if (interactive) {
            report_interactive(*panel, printed, last_files_count);
        }
    });
    app.run();

    if (!interactive) {
        emit("");
        emit("Automated section done. To verify against real OS drag sessions, rerun with --interactive.");
    }
    return failures == 0 ? 0 : 4;
}
