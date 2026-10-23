/* X11 Xdnd 拖入 / 拖出桥 —— 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖接缝：
//   拖入：`XdndAware` 广告 + `XdndEnter`/`XdndPosition`/`XdndLeave`/`XdndDrop` 消息 →
//   `detail::X11DragBridge` → 折算层（`detail/system_drag_translate`）→ 拖入入口
//   （`event/detail/system_drag_ingress`）→ `EventDispatcher` → 控件
//   `query_system_drag_accept` / `on_system_drop`，以及 `XdndStatus` 回写的动作协商闭环；
//   数据经 selection round-trip（`XConvertSelection` → `SelectionNotify`）落进 `DragPayload`。
//   拖出：控件 `begin_system_drag()` → `system_drag::begin` → 拖出后端（平台桥经
//   `detail::register_system_drag_out_backend` 登记）→ 指针 grab + `XdndSelection` 所有权
//   + 消息序列；终态经 `complete_drag_out_session` 登记、帧循环 `pump_reads()` 投递
//   `on_system_drag_end`。
//
// 为什么必须真机：无头 CI 里没有 X 服务器，`XdndAware` 无从广告、OS 会话回调永不到达，
// 而「跨进程会话」这件事本身不存在——裸 `Window` 也没有 handler，只有 `Application` 登记的
// `WindowHost` 才把后端事件接进 `EventDispatcher`。单元测试覆盖的是平台中立部分（折算层
// `utest_system_drag_translate`、拖入入口 `utest_system_drag_ingress`）；本探针覆盖的是
// 「桥与 X 服务器的接缝」这一条无头证明不了的部分。
//
// 自动段（无需人工拖文件）逐项验收：
//   ① 能力位：本机构建下 `platform().capabilities().system_drag_in` / `system_drag_out` 均为 true。
//   ② 广告：窗口上确实存在 `XdndAware`（XA_ATOM / 32 / 版本 ≥ 1）—— 这是「宿主确实装了
//      桥」的反向证明；属性不存在即桥未 install。
//   ③ 派发链：经 `EventDispatcher` 投一条合成 Enter 事件，accept 查询发生且 `allowed` 为
//      proposed 与 accept 的求交结果（真机构建下派发语义与无头单测一致）。
//   ④ 自拖放全链路（无人值守）：指针泊在落放面板上 → `begin_system_drag()` 发起会话 →
//      桥 grab 指针、取得 `XdndSelection` 所有权、探到落点（本窗口自身）→ 自发 `XdndEnter`
//      / `XdndPosition` → 本窗口作为目标回 `XdndStatus` → 探针合成一次 `ButtonRelease`
//      → `XdndDrop` → 目标侧 selection round-trip 向本进程索取数据 → `provide` 应答
//      → 派发 `Drop`（载荷与要约文本一致）→ `XdndFinished` 定终局 → 帧循环投递
//      `on_system_drag_end(Succeeded, Copy)`。
//
// 人工段（`--interactive`）：窗口常驻，逐帧打印拖入四态与拖出三态。
//   拖入：从文件管理器拖一个或多个文件进来，逐项肉眼核对：① 进入窗口即有 Enter、移动时
//   持续 Over、移出有 Leave、放下有 Drop；② Drop 后打印的文件路径与拖入的文件一致
//   （uri-list 解码往返）；③ 按住 Ctrl / Shift 时 proposed 随之变化。
//   拖出：从下方拖源面板按住左键拖到本窗外（文本编辑器 / 文件管理器均可）后松开，期望
//   begin → end(Succeeded, Copy)；拖动中按 Esc，期望 begin → cancel → end(Cancelled, None)。
//
// 构建：
//   cmake -S . -B build-verify-drag -G Ninja -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify-drag --target aurora_verify_x11_drag
//   ./build-verify-drag/aurora_verify_x11_drag [--interactive]
//
// 退出码：
//   0  自动段全部通过
//   2  环境不可用（无 DISPLAY / 建窗失败）
//   3  自动段无失败，但自拖放腿因环境限制未跑（Wayland/Xwayland 下指针泊不进本窗口）
//   4  部分验收项不符 —— 见逐行 PASS/FAIL
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
#error "aurora_verify_x11_drag can only be built on desktop Linux (AURORA_PLATFORM_LINUX)"
#endif

#ifndef AURORA_BACKEND_X11
#error "AURORA_BACKEND_X11 must be enabled"
#endif

// Xlib 必须排在**全部 aurora 头之后**：它把 `None` 定义成对象宏，先包含会让
// `aurora::DragAction::None` / `ModifierKey::None` 被展开成 `::0L`（枚举定义整体损坏，
// 症状是「DragAction 未声明」这类看似无关的报错）。取值后立即 #undef，纪律同
// src/aurora/window/x11_surface.cpp。
//
// 整段包含用 `clang-format off` 圈住：格式化器按「系统头优先」重排包含时会把
// `<X11/...>` 提到 aurora 头之前，上述污染随即发生。顺序是本文件的语义，不由排版器决定。
// clang-format off
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "verify_args.h"
#include "verify_print.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
// clang-format on

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// 豁免范围同 src/aurora/window/detail/x11_drag.cpp：XEvent 的 xclient.data.l / xselection 是 X
// 协议固有的 tagged union 字段，Xlib 属性读写以 unsigned char* 为字节口。

namespace {
// Xlib 的 None 宏与 aurora 的 `*.None` 枚举器冲突：先取值再解除宏定义。
constexpr ::Window AURORA_X_NONE = None;
}  // namespace
#undef None

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
/// 同时充当落点——指针泊在本面板上，桥探到的 Xdnd 目标就是本窗口自身。
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
        e.is_handled = true;  // 消费即受理：目标侧据此回写 `XdndFinished` 的受理位
    }

    [[nodiscard]] auto query_system_drag_accept([[maybe_unused]] const std::vector<std::string> &formats)
        -> aurora::DragAction override {
        ++accept_queries;
        return accept;
    }
};

// 探针布局（固定窗口 480x300）：上 480x200 落放面板 + 下 480x100 拖源面板；自拖放泊点取
// 落放面板中心（客户区 240,100），与两块面板 on_layout 的请求尺寸对应。
constexpr int AURORA_PROBE_PARK_X = 240;
constexpr int AURORA_PROBE_PARK_Y = 100;

// 自拖放要约给出的文本：自动段断言「落放载荷与 provide 给出的文本一致」（要约 → selection
// round-trip → 拖入侧载荷的往返）。
constexpr const char AURORA_DRAG_OUT_PROBE_TEXT[] = "aurora x11 drag-out self-drop payload";

// 自拖放终态的墙钟截止：正常路径在十数帧内完成（全链路都在本进程与 X 服务器之间往返）；
// 20s 是给慢机 / 远程桌面的余量，超时即判 FAIL 并打印，不悬挂。
constexpr auto AURORA_DRAG_OUT_DEADLINE = std::chrono::seconds{20};

// 等窗口可见的墙钟上限（reparenting WM 完成映射通常在数百毫秒内；超时即判环境不可用）。
constexpr auto AURORA_VIEWABLE_DEADLINE = std::chrono::seconds{10};

// 拖入腿（外部源注入）的墙钟截止：全链路都在本进程与 X 服务器之间往返，数帧内即完成。
constexpr auto AURORA_DRAG_IN_DEADLINE = std::chrono::seconds{15};

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
/// 自拖放（不经过真实指针事件，落放由探针合成的 `ButtonRelease` 触发）。
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
            const std::string text = AURORA_DRAG_OUT_PROBE_TEXT;
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

/// @brief 读回窗口上的 `XdndAware`（判据 ②）：装了桥就该有，未装则属性不存在。
/// @return 广告的协议版本；未广告时 0。
auto read_xdnd_aware(Display *dpy, ::Window win) -> int {
    const Atom aware = XInternAtom(dpy, "XdndAware", True);  // True = 只查已存在的 atom
    if (aware == AURORA_X_NONE) {
        return 0;
    }
    Atom actual_type = AURORA_X_NONE;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, win, aware, 0, 1, False, XA_ATOM, &actual_type, &actual_format, &nitems, &after,
                           &prop) != Success) {
        return 0;
    }
    int version = 0;
    if (prop != nullptr) {
        if (actual_type == XA_ATOM && actual_format == 32 && nitems >= 1) {
            // Xlib 属性以 unsigned char* 为字节口：整块拷出再取值（对齐由 Xlib 保证）。
            unsigned long value = 0;
            std::memcpy(&value, prop, sizeof(value));
            version = static_cast<int>(value);
        }
        XFree(prop);
    }
    return version;
}

/// @brief 把指针泊到窗口客户区的指定点（自拖放发起前：桥据此探到本窗口为落点）。
auto park_pointer(Display *dpy, ::Window win, int x, int y) -> void {
    XWarpPointer(dpy, AURORA_X_NONE, win, 0, 0, 0, 0, x, y);
    XSync(dpy, False);
}

/// @brief 合成一次 `ButtonRelease` 投给本窗口（拖出会话的落放触发点）。
///
/// 为什么可以合成：拖放会话的「释放」在 X 下就是一条 `ButtonRelease` 事件，桥只在事件泵里
/// 认这一条；探针不按任何真实按键，故不会干扰桌面。`event_mask` 传 0 = 交给目标窗口的创建者
/// （本进程），非零则在服务器侧另寻对该掩码感兴趣的客户，反而投不到自己。
auto send_button_release(Display *dpy, ::Window win, int x, int y) -> void {
    int rx = x;
    int ry = y;
    ::Window child = AURORA_X_NONE;
    if (XTranslateCoordinates(dpy, win, XDefaultRootWindow(dpy), x, y, &rx, &ry, &child) == False) {
        rx = x;
        ry = y;
    }
    XEvent ev{};
    ev.type = ButtonRelease;
    ev.xbutton.display = dpy;
    ev.xbutton.window = win;
    ev.xbutton.root = XDefaultRootWindow(dpy);
    ev.xbutton.subwindow = AURORA_X_NONE;
    ev.xbutton.time = CurrentTime;  // 服务器按当前时间受理（协议允许）
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    ev.xbutton.x_root = rx;
    ev.xbutton.y_root = ry;
    ev.xbutton.state = 0U;
    ev.xbutton.button = 1U;
    ev.xbutton.same_screen = True;
    XSendEvent(dpy, win, False, 0, &ev);
    XFlush(dpy);
}

/// @brief 窗口是否处于可查看态（XGrabPointer 的前置条件：`GrabNotViewable` 即窗口未映射完成）。
///
/// 为什么必须等：XMapWindow 是异步请求，reparenting WM 还要把窗口挂进框架窗口再映射，故建窗
/// 后若干帧内 `map_state` 仍不是 `IsViewable`，此刻 grab 必失败——不是桥的缺陷，是时序。
auto window_viewable(Display *dpy, ::Window win) -> bool {
    XWindowAttributes attr{};
    if (XGetWindowAttributes(dpy, win, &attr) == 0) {
        return false;
    }
    return attr.map_state == IsViewable;
}

/// @brief 指针是否确实停在本窗口上（`XQueryPointer` 的 child 链上溯能找到本窗口）。
///
/// 为什么必须校验：`XQueryPointer` 返回的是指针下**最深的子窗**，窗口被别的桌面窗口盖住时
/// 它是遮挡者，桥随即会把 Xdnd 会话开到那个无关窗口上（它不回 `XdndStatus`，落放必然被拒）。
auto pointer_over_window(Display *dpy, ::Window win) -> bool {
    ::Window root = AURORA_X_NONE;
    ::Window child = AURORA_X_NONE;
    int rx = 0;
    int ry = 0;
    int cx = 0;
    int cy = 0;
    unsigned int mask = 0;
    if (XQueryPointer(dpy, XDefaultRootWindow(dpy), &root, &child, &rx, &ry, &cx, &cy, &mask) == False) {
        return false;
    }
    for (int i = 0; i < 32 && child != AURORA_X_NONE; ++i) {
        if (child == win) {
            return true;
        }
        ::Window parent = AURORA_X_NONE;
        ::Window tree_root = AURORA_X_NONE;
        ::Window *kids = nullptr;
        unsigned int count = 0;
        if (XQueryTree(dpy, child, &tree_root, &parent, &kids, &count) == 0) {
            break;
        }
        if (kids != nullptr) {
            XFree(kids);
        }
        child = parent;
    }
    return false;
}

/// @brief 探针扮演的「外部 Xdnd 源」：自带窗口作 `XdndSelection` 属主，向本窗口发会话消息。
///
/// 为什么不用库自身的拖出去驱动拖入：那条腿要靠 `XQueryPointer` 探到「指针下的窗口」，而
/// Wayland + Xwayland（rootless）下 X 服务器拿不到真实窗口栈，探到的会是无关窗口。本腿反过来
/// —— 探针以**另一条 X 连接**（等价另一个客户端 / 另一个进程）的身份直接向本窗口发 Xdnd 消息
/// 并应答 `SelectionRequest`，于是拖入全链路（Enter → Position → Status 协商 → Drop → selection
/// 取数 → Finished 回写）都能验收，且完全不依赖窗口栈与真实指针。
struct ExternalSource {
    ::Window window = AURORA_X_NONE;  ///< 属主窗口（刻意建在屏幕外且不映射：只作属主身份）
    unsigned long selection = 0;  ///< `XdndSelection`
    unsigned long enter = 0;  ///< `XdndEnter`
    unsigned long position = 0;  ///< `XdndPosition`
    unsigned long status = 0;  ///< `XdndStatus`
    unsigned long drop = 0;  ///< `XdndDrop`
    unsigned long finished = 0;  ///< `XdndFinished`
    unsigned long target = 0;  ///< 本腿提供的目标名（`text/plain;charset=utf-8`）
    unsigned long action_copy = 0;  ///< `XdndActionCopy`
    bool status_seen = false;  ///< 是否收到目标回的 `XdndStatus`
    bool status_accepts = false;  ///< `XdndStatus` 的受理位
    aurora::DragAction status_action = aurora::DragAction::None;  ///< `XdndStatus` 商定的动作
    bool finished_seen = false;  ///< 是否收到目标回的 `XdndFinished`
    bool finished_accepted = false;  ///< `XdndFinished` 的受理位
    aurora::DragAction finished_action = aurora::DragAction::None;  ///< `XdndFinished` 报告的执行动作
    int selection_requests = 0;  ///< 目标发来的取数请求次数
};

/// @brief 建外部源身份：intern 协议 atom、建属主窗口、取得 `XdndSelection` 所有权。
auto source_init(ExternalSource &src, Display *dpy) -> bool {
    src.selection = static_cast<unsigned long>(XInternAtom(dpy, "XdndSelection", False));
    src.enter = static_cast<unsigned long>(XInternAtom(dpy, "XdndEnter", False));
    src.position = static_cast<unsigned long>(XInternAtom(dpy, "XdndPosition", False));
    src.status = static_cast<unsigned long>(XInternAtom(dpy, "XdndStatus", False));
    src.drop = static_cast<unsigned long>(XInternAtom(dpy, "XdndDrop", False));
    src.finished = static_cast<unsigned long>(XInternAtom(dpy, "XdndFinished", False));
    src.target = static_cast<unsigned long>(XInternAtom(dpy, "text/plain;charset=utf-8", False));
    src.action_copy = static_cast<unsigned long>(XInternAtom(dpy, "XdndActionCopy", False));
    if (src.window == AURORA_X_NONE) {
        src.window = XCreateSimpleWindow(dpy, XDefaultRootWindow(dpy), -100, -100, 8, 8, 0, 0, 0);
    }
    XSetSelectionOwner(dpy, static_cast<Atom>(src.selection), src.window, CurrentTime);
    XFlush(dpy);
    return XGetSelectionOwner(dpy, static_cast<Atom>(src.selection)) == src.window;
}

/// @brief 以外部源身份发一条 32 位 ClientMessage 给目标窗口。
auto source_send(Display *dpy, ::Window dest, unsigned long type, long l0, long l1, long l2, long l3, long l4) -> void {
    XEvent ev{};
    ev.type = ClientMessage;
    ev.xclient.display = dpy;
    ev.xclient.window = dest;
    ev.xclient.message_type = static_cast<Atom>(type);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = l0;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(dpy, dest, False, 0, &ev);
    XFlush(dpy);
}

/// @brief 发 `XdndEnter`：协议版本 5、单一类型（不置 `XdndTypeList` 位）。
auto source_send_enter(ExternalSource &src, Display *dpy, ::Window dest) -> void {
    const long flags = static_cast<long>(5) << 24;
    source_send(dpy, dest, src.enter, static_cast<long>(src.window), flags, static_cast<long>(src.target), 0L, 0L);
}

/// @brief 发 `XdndPosition`：根坐标 + 时间戳 + 源建议动作（Copy）。
auto source_send_position(ExternalSource &src, Display *dpy, ::Window dest, int x_root, int y_root) -> void {
    const long packed = (static_cast<long>(x_root & 0xFFFF) << 16) | static_cast<long>(y_root & 0xFFFF);
    source_send(dpy, dest, src.position, static_cast<long>(src.window), 0L, packed, CurrentTime,
                static_cast<long>(src.action_copy));
}

/// @brief 发 `XdndDrop`：请求目标立即取数。
auto source_send_drop(ExternalSource &src, Display *dpy, ::Window dest) -> void {
    source_send(dpy, dest, src.drop, static_cast<long>(src.window), 0L, CurrentTime, 0L, 0L);
}

/// @brief 客户区坐标 → 根坐标（发 `XdndPosition` 需要根坐标：协议规定如此）。
auto window_to_root(Display *dpy, ::Window win, int x, int y, int *rx, int *ry) -> void {
    ::Window child = AURORA_X_NONE;
    if (XTranslateCoordinates(dpy, win, XDefaultRootWindow(dpy), x, y, rx, ry, &child) == False) {
        *rx = x;
        *ry = y;
    }
}

/// @brief 泵探针连接的事件：`XdndStatus` / `SelectionRequest` / `XdndFinished` 三条回程。
auto source_pump(ExternalSource &src, Display *dpy) -> void {
    while (XPending(dpy) > 0) {
        XEvent ev{};
        XNextEvent(dpy, &ev);
        if (ev.type == ClientMessage && static_cast<unsigned long>(ev.xclient.message_type) == src.status) {
            const auto flags = static_cast<unsigned long>(ev.xclient.data.l[1]);
            src.status_seen = true;
            src.status_accepts = (flags & 0x1UL) != 0UL;
            const auto action = static_cast<unsigned long>(ev.xclient.data.l[4]);
            src.status_action = action == src.action_copy ? aurora::DragAction::Copy : aurora::DragAction::None;
        } else if (ev.type == ClientMessage && static_cast<unsigned long>(ev.xclient.message_type) == src.finished) {
            const auto flags = static_cast<unsigned long>(ev.xclient.data.l[1]);
            src.finished_seen = true;
            src.finished_accepted = (flags & 0x1UL) != 0UL;
            const auto action = static_cast<unsigned long>(ev.xclient.data.l[2]);
            src.finished_action = action == src.action_copy ? aurora::DragAction::Copy : aurora::DragAction::None;
        } else if (ev.type == SelectionRequest) {
            ++src.selection_requests;
            const auto target = static_cast<unsigned long>(ev.xselectionrequest.target);
            const auto property = static_cast<unsigned long>(ev.xselectionrequest.property);
            const std::string text = AURORA_DRAG_OUT_PROBE_TEXT;
            if (target == src.target) {
                // XChangeProperty 的数据形参是 `const unsigned char*`：给一份字节副本即可，
                // 无需 const_cast / reinterpret_cast。
                const std::vector<unsigned char> bytes(text.begin(), text.end());
                XChangeProperty(dpy, ev.xselectionrequest.requestor, static_cast<Atom>(property),
                                static_cast<Atom>(target), 8, PropModeReplace, bytes.data(),
                                static_cast<int>(bytes.size()));
            }
            // 无论给不给数据都要回执（拒绝时 property 为 None，此处按已写入回执）。
            XEvent reply{};
            reply.type = SelectionNotify;
            reply.xselection.display = dpy;
            reply.xselection.requestor = ev.xselectionrequest.requestor;
            reply.xselection.selection = ev.xselectionrequest.selection;
            reply.xselection.target = ev.xselectionrequest.target;
            reply.xselection.property =
                target == src.target ? ev.xselectionrequest.property : static_cast<Atom>(AURORA_X_NONE);
            reply.xselection.time = ev.xselectionrequest.time;
            XSendEvent(dpy, ev.xselectionrequest.requestor, False, 0, &reply);
            XFlush(dpy);
        }
    }
}

/// @brief 拖入腿（外部源）的断言收尾。
auto settle_drag_in_leg(const ExternalSource &src, DropPanel &panel, const int accept_queries_before, int &failures)
    -> void {
    check(src.status_seen, "target answers XdndStatus (action negotiation reached the source)", failures);
    check(src.status_accepts, "XdndStatus reports acceptance (allowed propagated back to the source)", failures);
    check(src.status_action == aurora::DragAction::Copy,
          std::string{"XdndStatus carries the negotiated action Copy (got "} + action_name(src.status_action) + ")",
          failures);
    check(panel.accept_queries > accept_queries_before, "accept query happened for the injected session", failures);
    const std::vector<std::string> &log = panel.phase_log;
    check(std::find(log.begin(), log.end(), "Enter") != log.end(), "Enter phase reached the widget", failures);
    check(std::find(log.begin(), log.end(), "Drop") != log.end(), "Drop phase reached the widget", failures);
    check(src.selection_requests == 1, "target requests exactly the one format it registered", failures);
    check(panel.last_text == AURORA_DRAG_OUT_PROBE_TEXT,
          std::string{"drop payload round-trips the source text (got \""} + panel.last_text + "\")", failures);
    check(src.finished_seen, "target answers XdndFinished", failures);
    check(src.finished_accepted, "XdndFinished reports the drop was consumed (is_handled propagated back)", failures);
}

/// @brief 自动段判据（真机环境 + 已建窗口）。
auto run_automated(Display *dpy, ::Window win, const aurora::Platform &plat, DropPanel &panel, int &failures) -> void {
    // ① 能力位：X11 Surface 的两个方向同批接线（拖入 + 拖出）。
    check(plat.surface == aurora::SurfaceKind::X11, "surface kind is X11 (Xdnd backend family)", failures);
    check(plat.capabilities().system_drag_in, "capabilities().system_drag_in is true after wiring", failures);
    check(plat.capabilities().system_drag_out, "capabilities().system_drag_out is true after wiring", failures);

    // ② 广告：窗口上必须有 `XdndAware`（桥 install 的直接后果）。属性不存在 = 桥未装，
    //    外部源永远看不到本窗口是拖放目标，③ 之后的全部判据都不成立。
    const int version = read_xdnd_aware(dpy, win);
    check(version >= 1, "window advertises XdndAware (version=" + std::to_string(version) + ")", failures);

    // ③ 派发链：合成一条 Enter 事件经真派发器，accept 查询发生且 allowed 为求交结果。
    aurora::SystemDropEvent event;
    event.phase = aurora::SystemDropPhase::Enter;
    event.position =
        aurora::Point{.x = static_cast<float>(AURORA_PROBE_PARK_X), .y = static_cast<float>(AURORA_PROBE_PARK_Y)};
    event.formats.emplace_back(aurora::AURORA_MIME_URI_LIST);
    event.proposed = aurora::DragAction::Move;
    panel.accept = aurora::DragAction::Move;
    const bool dispatched = aurora::EventDispatcher::dispatch(panel, event);
    check(dispatched, "synthetic Enter event reaches the hit target through the real dispatcher", failures);
    check(panel.accept_queries > 0, "accept query happens on Enter (action negotiation has a source)", failures);
    check(event.allowed == aurora::DragAction::Move, "allowed is the intersection of proposed and accepted", failures);
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
          "self-drop reached this window's Xdnd target (Drop phase)", failures);
    check(panel.last_text == AURORA_DRAG_OUT_PROBE_TEXT,
          std::string{"drop payload round-trips the offered text/plain (got \""} + panel.last_text + "\")", failures);
}

/// @brief 人工段：逐帧打印新增的四态与最近一次落放内容。
auto report_interactive(DropPanel &panel, std::size_t &printed, int &last_files_count, int &last_text_count) -> void {
    if (panel.phase_log.size() > printed) {
        for (std::size_t i = printed; i < panel.phase_log.size(); ++i) {
            emit(std::string("[drag] phase=") + panel.phase_log[i] + " formats=" +
                 std::to_string(panel.last_formats.size()) + " allowed=" + action_name(panel.last_allowed));
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
    // 文本长度单独一行：大载荷（超过同步阈值、走 ICCCM INCR 分块）在人工段只能靠字符数
    // 判断有没有被截断——打印全文会把 stdout 冲垮，且肉眼也数不清。
    const int chars = static_cast<int>(panel.last_text.size());
    if (chars != last_text_count) {
        last_text_count = chars;
        emit(std::string("[drag] drop text=") + std::to_string(chars) + " chars");
    }
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

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码呈现（捕获反而把它压成 0），与 demo 入口同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("X11 Xdnd drag-in / drag-out bridge live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");
    emit("==== X11 Xdnd drag bridge live acceptance (drag-in + drag-out) ====");

    // 探针自带一条 X 连接：读 `XdndAware`、泊指针、合成释放键都经由它。与 Surface 内部的连接
    // 是两条独立连接，但操作的是同一个 X 服务器与同一个窗口，事件投递与属性读写都成立。
    Display *dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        AURORA_LOG_ERROR("verify", "cannot open X display (DISPLAY unset or unreachable)");
        return 2;
    }

    auto panel = std::make_shared<DropPanel>();
    auto source = std::make_shared<DragSourcePanel>();
    panel->layout(aurora::Constraints{}, aurora::BuildContext{});
    source->layout(aurora::Constraints{}, aurora::BuildContext{});
    // 置顶（X11 下经 `_NET_WM_STATE_ABOVE`）：自拖放要以「指针下的最深窗口就是本窗口」为前提，
    // 被桌面上的其它窗口盖住时 `XQueryPointer` 返回的 child 是遮挡者，会话会落到无关窗口上。
    aurora::WindowStyleOptions probe_style{};
    probe_style.always_on_top = true;
    auto made = aurora::create_native_window(aurora::WindowOptions{
        .size = aurora::Size{.width = 480.0F, .height = 300.0F}, .title = "Aurora drag verify", .style = probe_style});
    if (!made) {
        AURORA_LOG_ERROR("verify", "cannot create native window");
        return 2;
    }
    aurora::Window *window = made.value().get();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) 公共 API 的 void* 句柄 → XID
    const auto win = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window->surface().native_handle()));
    if (win == 0) {
        AURORA_LOG_ERROR("verify", "cannot obtain X window id");
        return 2;
    }
    const aurora::Platform plat = aurora::platform();
    // 两种模式都不设帧预算：自动段由帧回调在终态到达 / 截止时自行 quit。power_saving 关掉是
    // 为了保证自拖放期间帧循环持续泵平台消息——X 会话回调经 X 事件队列到达，空闲阻塞等待
    // 会把落放消息的派发推迟到截止之后。
    aurora::Application app(aurora::Scene{aurora::Node{aurora::Column(
                                aurora::ColumnProps{.children = {aurora::Node{panel}, aurora::Node{source}}})}},
                            std::move(made.value()),
                            aurora::WindowOptions{.size = aurora::Size{.width = 480.0F, .height = 300.0F},
                                                  .title = "Aurora drag verify",
                                                  .max_frames = -1,
                                                  .style = probe_style,
                                                  .power_saving = false});

    int frame = 0;
    int failures = 0;
    std::size_t printed = 0;
    std::size_t printed_out = 0;
    int last_files_count = 0;
    int last_text_count = 0;
    bool drag_out_armed = false;
    bool release_sent = false;
    bool drag_out_settled = false;
    bool pending_manual = false;  // 自拖放腿因环境限制未跑（退出码 3，不冒充 PASS）
    int accept_queries_before = 0;
    int armed_frame = 0;
    bool leg_in_armed = false;  // 拖入腿（外部源注入）是否已发起
    bool leg_in_settled = false;  // 拖入腿是否已结算
    bool leg_in_drop_sent = false;  // `XdndDrop` 是否已发（收到受理态之后）
    std::chrono::steady_clock::time_point leg_in_began{};
    ExternalSource ext_src{};  // 外部源身份（另一条 X 连接）
    std::chrono::steady_clock::time_point drag_out_began{};
    const std::chrono::steady_clock::time_point armed_wait_from = std::chrono::steady_clock::now();
    app.set_on_frame([&]() -> void {
        ++frame;
        if (frame == 3) {
            run_automated(dpy, win, plat, *panel, failures);
            if (!interactive) {
                emit("");
                emit("---- Automated drag-in leg: this process acts as a foreign Xdnd source ----");
                emit("The probe opens its own X connection, takes XdndSelection ownership and drives a");
                emit("full session against this window: Enter -> Position -> Status -> Drop ->");
                emit("selection round-trip -> Finished. No pointer stacking is involved.");
            } else {
                emit("");
                emit("---- Manual section ----");
                emit("Drag-in: drag one or more files from the file manager into the upper panel.");
                emit("Expect: Enter on entry -> repeated Over while moving -> Leave on exit -> Drop on");
                emit("        release; dropped paths are listed below. Hold Ctrl / Shift to see the");
                emit("        proposed action change.");
                emit("Drag-out: press and hold the left button on the lower panel, drag outside this");
                emit("        window (text editor / file manager), then release: expect begin ->");
                emit("        end(Succeeded, Copy). Press Esc mid-drag: begin -> cancel ->");
                emit("        end(Cancelled, None).");
                emit("(no automated verdict in this section; closing the window exits)");
            }
        }
        // ---- 拖入腿：探针以「另一条 X 连接」的身份注入一场完整 Xdnd 会话 ----
        if (!interactive && frame == 6 && !leg_in_armed) {
            panel->accept = aurora::DragAction::Copy;  // 判据 ③ 借去过 Move，落放腿改回 Copy
            accept_queries_before = panel->accept_queries;
            if (!source_init(ext_src, dpy)) {
                check(false, "probe takes XdndSelection ownership (external source identity)", failures);
                leg_in_armed = true;
                leg_in_settled = true;
            } else {
                source_send_enter(ext_src, dpy, win);
                int rx = 0;
                int ry = 0;
                window_to_root(dpy, win, AURORA_PROBE_PARK_X, AURORA_PROBE_PARK_Y, &rx, &ry);
                source_send_position(ext_src, dpy, win, rx, ry);
                leg_in_armed = true;
                leg_in_began = std::chrono::steady_clock::now();
            }
        }
        if (!interactive && leg_in_armed && !leg_in_settled) {
            source_pump(ext_src, dpy);  // 收 Status / SelectionRequest / Finished 三条回程
            if (ext_src.status_seen && !leg_in_drop_sent) {
                leg_in_drop_sent = true;  // 目标已表态受理：按协议发 Drop，让它立刻取数
                source_send_drop(ext_src, dpy, win);
            }
            if (ext_src.finished_seen) {
                settle_drag_in_leg(ext_src, *panel, accept_queries_before, failures);
                leg_in_settled = true;
            } else if (std::chrono::steady_clock::now() - leg_in_began > AURORA_DRAG_IN_DEADLINE) {
                check(false, "injected session settles within the deadline", failures);
                emit("[info] drag-in leg timed out (XdndFinished never arrived?)");
                leg_in_settled = true;
            }
        }
        // ---- 拖出腿：库侧发起的自拖放（需要指针能泊进本窗口）----
        // 自拖放发起前先等窗口真正可见（`XGrabPointer` 在 `GrabNotViewable` 下必失败）。
        if (!interactive && leg_in_settled && !drag_out_armed && !drag_out_settled) {
            if (!window_viewable(dpy, win) || !pointer_over_window(dpy, win)) {
                XRaiseWindow(dpy, win);
                park_pointer(dpy, win, AURORA_PROBE_PARK_X, AURORA_PROBE_PARK_Y);
                if (std::chrono::steady_clock::now() - armed_wait_from > AURORA_VIEWABLE_DEADLINE) {
                    // 记 PENDING 而非 PASS/FAIL：`XQueryPointer` 在 Wayland + Xwayland（rootless）
                    // 下拿不到真实窗口栈，指针泊不进本窗口是环境限制而非桥的缺陷。用别的窗口
                    // 冒充落点会让本条判据恒真，故如实挂起，交 --interactive 人工段。
                    emit("[PENDING] self drag-out leg skipped: the pointer cannot be parked over this");
                    emit("          window (Wayland/Xwayland rootless or the window is occluded).");
                    emit("          Re-run with --interactive on a real X11 session (Xorg / Xvfb).");
                    pending_manual = true;
                    app.quit();
                }
                return;
            }
            emit("");
            emit("---- Automated drag-out leg: self drag-out onto this window ----");
            park_pointer(dpy, win, AURORA_PROBE_PARK_X, AURORA_PROBE_PARK_Y);
            const auto started = source->begin_system_drag();
            check(static_cast<bool>(started), "begin_system_drag() starts the session (Ok)", failures);
            drag_out_armed = true;
            armed_frame = frame;
            drag_out_began = std::chrono::steady_clock::now();
            return;
        }
        // 释放键留几帧余量：`XdndEnter` / `XdndPosition` / `XdndStatus` 三趟消息都要先在事件泵里
        // 走完，届时目标（本窗口）才处于「接受」态，释放才会落成一次真正的 drop。
        if (!interactive && drag_out_armed && !release_sent && frame >= armed_frame + 5) {
            release_sent = true;
            send_button_release(dpy, win, AURORA_PROBE_PARK_X, AURORA_PROBE_PARK_Y);
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
                app.quit();
            } else if (elapsed > AURORA_DRAG_OUT_DEADLINE) {
                check(false, "self drag-out terminal state delivered within the deadline", failures);
                emit("[info] drag-out leg timed out with no terminal state (Xdnd session never settled?)");
                drag_out_settled = true;
                app.quit();
            }
        }
        if (interactive) {
            report_interactive(*panel, printed, last_files_count, last_text_count);
            report_drag_out(*source, printed_out);
        }
    });
    app.run();

    if (!interactive) {
        emit("");
        emit("Automated section done. To verify against real OS drag sessions, rerun with --interactive.");
    }
    if (failures > 0) {
        return 4;
    }
    // 无失败但有判据挂起：退出码 3（PENDING MANUAL），与既有探针的「人工未做」语义同款。
    return pending_manual ? 3 : 0;
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTEND(cppcoreguidelines-pro-type-union-access)
