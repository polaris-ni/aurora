#pragma once

// ============================================================
// OS 级输入注入通道 · 真机增强层（tools/include/e2e/os_input.h）
// ------------------------------------------------------------
// 与内核的目标式注入（`Session::tap` 等，经 `Inspector::simulate_*` 走进程内命中测试）
// 互补：本头把**真实 OS 输入**投递进系统，证明「OS 输入 → 窗口过程 → Aurora 事件管线」
// 整段接线，而非仅后半段。三个平台原生通道：
//   - Windows：`SendInput`（全局注入——移动真实光标后投递，投给光标所在窗口）与
//     `PostMessage`（定向投递——不经光标/焦点系统，直达窗口过程）；
//   - X11：XTest 扩展（经 X 服务器合成真实输入事件；dlopen libX11/libXtst 免构建依赖，
//     与 `aurora_verify_x11_ime_live` 探针同款）；
//   - macOS：CGEvent（`CGEventPost(kCGSessionEventTap)` 走真实 OS 输入流，**须辅助功能
//     授权**；未授权时该路一个事件都不送达，故为「已授权的加强腿」，默认证据由
//     `aurora_verify_macos_os_input` 探针在 AppKit 事件队列注入上取证）；
//   - WASM：CDP `Input.dispatch*`（浏览器外部驱动，见 tools/verify/wasm_input_cdp_drive.mjs，
//     真实浏览器 opt-in，与 a11y CDP 通道同口径）。
//
// 门控与策略语义（对应 specification/08-tooling.md §8.2）：
//   1. **opt-in 环境变量**（`AURORA_E2E_OS_INPUT`）：真机增强层显式选择加入。未置位时
//      用例为 skip 桩，在默认 `ctest` 中天然不参与并行。SendInput 是全局注入、无法定向
//      窗口，多窗口并存时必然串台——该用例组须以**独立 ctest 调用**按 stem 独占执行
//      （`ctest -L e2e -R etest_os_input`），**不使用 `RUN_SERIAL` 属性**（TEST-R7 禁止）：
//      既有模型「进程隔离 + 资源虚拟化」隔离的是资源，全局输入是无法虚拟化的共享设备。
//   2. **no_interactive_desktop 拒绝语义**（`interactive_desktop()`）：锁定/安全桌面/
//      服务会话等非交互桌面上注入通道以明确原因跳过，不静默失败、不挂起、不误判通过。
//   3. **窗口策略须 `Normal`**：OS 输入投递到完全隐藏的窗口在 Win32 上语义不成立
//      （SendInput 投给光标所在窗口，隐藏窗口不在命中路径）——用例侧不得改用 `Hidden`。
//   4. **SendInput 落点守卫**（`sendinput_landing_check`）：全局注入投给光标所在窗口，
//      故注入前先 best-effort 抬升目标窗口、再按屏幕落点复核命中的确是目标；桌面被其他
//      程序占用且前台锁定拒绝夺焦时带原因拒绝，用例侧据此记 skip——环境竞争不是管线故障
//      （同坐标的 `post_click` 通道仍独立取证）。
//   5. **用例侧不得调用 `Surface::set_event_handler`**：该槽是宿主的派发接线本身，写入即
//      **覆盖** `WindowHost` 装上的分发器（实测：装上计数 handler 后窗口存活、事件仍被
//      出队，但控件状态纹丝不动——派发落空）。要读派发结果请用**外部可读的实值**：控件
//      语义状态（`Session::semantic_snapshot`）与帧像素（`Session::read_pixels`）。
//
// 本头与 harness.h 同纪律：只依赖 aurora 公共头、不含测试框架宏；通道缺失（平台不支持、
// dlopen 失败、系统授权未授予）以 `Result` 错误或探测函数如实上报，**不是构建失败**。
// 覆盖面与 a11y 通道同口径：win32/d3d11（共用 Win32Host 输入管线，以 win32 为代表）与 x11，
// macOS 以 macos 后端为代表（CGEvent 加强腿需授权；无授权时由探针在 AppKit 队列侧取证）；
// GLFW 不暴露原生句柄（`native_handle()` 为基类空实现）、Wayland 无 XTest 等价物，均记为已知缺口。
// ============================================================

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "e2e/harness.h"

#ifdef AURORA_PLATFORM_WINDOWS
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#ifndef NOMINMAX  // NOLINT(readability-identifier-naming)
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#endif

#ifdef AURORA_PLATFORM_MACOS
// CoreGraphics 事件族 + HIServices 的授权探测，全部是 C API（本头在纯 C++ 侧编译，无 ObjC
// 语法与 ARC 可用——与 `macos_ax_client.h` 同纪律，AppKit 队列注入那一腿因此落在 `.mm` 探针里）。
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace aurora::e2e {

/// @brief 真机增强层的 opt-in 环境变量名（置 `1`/`on`/`yes`/`true` 方启用，大小写不敏感）。
inline constexpr auto AURORA_OS_INPUT_ENV_VAR = "AURORA_E2E_OS_INPUT";

/// @brief 真机增强层是否已显式选择加入（首次读取后缓存；未置位 = skip 桩策略）。
///
/// 缓存安全的前提与 `expected_backends()` 相同：环境变量由编排方在进程启动前设定。
[[nodiscard]] inline auto os_input_opt_in() -> bool {
    static const bool ENABLED = [] {
        const char *raw = std::getenv(AURORA_OS_INPUT_ENV_VAR);
        if (raw == nullptr) {
            return false;
        }
        std::string value{raw};
        for (char &c : value) {
            c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        }
        return value == "1" || value == "on" || value == "yes" || value == "true";
    }();
    return ENABLED;
}

/// @brief 交互桌面探测结果（`interactive` 为 false 时 `reason` 携带明确原因）。
struct DesktopInteraction {
    bool interactive = false;
    std::string reason;  ///< 不可注入的原因（no_interactive_desktop 语义的载体）
};

/// @brief 当前会话是否运行在**可交互桌面**上（业界 `no_interactive_desktop` 语义）。
///
/// 锁定屏幕（WinSta0\Winlogon 安全桌面）、屏保、服务会话（Session 0）等场景下，全局
/// 注入要么被拒、要么投给无人看管的桌面——必须以明确原因拒绝，不得静默失败或挂起。
/// Windows：`OpenInputDesktop` 打不开输入桌面即非交互（锁定/安全桌面/服务会话）；实测
/// 本机锁屏态下该探测**仍会放行**（输入桌面名仍是 `Default`，而锁屏以全屏
/// `LockScreenBackstopFrame` 窗口盖住一切），此时 `SendInput` 不报错而是被该窗口静默吞掉
/// ——这类环境竞争由 `sendinput_landing_check` 的落点复核兜底，两道门互补而非互为替代。
/// Linux：无 X 显示（`DISPLAY` 未设）即无输入面；macOS：无 WindowServer 主显示即无输入面
/// （SSH 无会话登录 / 无头 CI 里 `CGMainDisplayID()` 为 0，事件无处可投）。授权与否不在本
/// 探测裁决，由 `cg_input_channel_available()` 单独把关——两件事的原因与解法都不同。
[[nodiscard]] inline auto interactive_desktop() -> DesktopInteraction {
#ifdef AURORA_PLATFORM_WINDOWS
    // 需要真实交互桌面：能打开输入桌面才允许全局注入。
    const HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desk == nullptr) {
        return DesktopInteraction{
            .interactive = false,
            .reason =
                "cannot open the input desktop (locked screen / secure desktop / service session); "
                "OS-level input injection would be silently dropped or misdelivered"};
    }
    CloseDesktop(desk);
    return DesktopInteraction{.interactive = true, .reason = ""};
#elif defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS)
    if (std::getenv("DISPLAY") == nullptr) {
        return DesktopInteraction{.interactive = false,
                                  .reason = "no X display (DISPLAY unset); XTest injection has no target server"};
    }
    return DesktopInteraction{.interactive = true, .reason = ""};
#elif defined(AURORA_PLATFORM_MACOS)
    const CGDirectDisplayID main_display = CGMainDisplayID();
    const CGRect bounds = CGDisplayBounds(main_display);
    if (main_display == 0U || bounds.size.width <= 0.0 || bounds.size.height <= 0.0) {
        return DesktopInteraction{.interactive = false,
                                  .reason =
                                      "no WindowServer main display (headless / ssh session without Aqua login); "
                                      "CGEvent injection has no screen to post onto"};
    }
    return DesktopInteraction{.interactive = true, .reason = ""};
#else
    return DesktopInteraction{.interactive = false,
                              .reason = "OS-level input channel not implemented on this platform (known gap)"};
#endif
}

// ============================================================
// Windows 通道（SendInput 全局注入 + PostMessage 定向投递）
// ============================================================
#ifdef AURORA_PLATFORM_WINDOWS

/// @brief 落点处窗口的类名（只用于诊断文案）：锁屏态实测是 `LockScreenBackstopFrame`，
///        把盖住落点的窗口写进原因，才能区分「用户正在用的窗口」与「锁屏吞输入」。
[[nodiscard]] inline auto window_class_of(HWND win) -> std::string {
    std::array<char, 64> buf{};
    if (GetClassNameA(win, buf.data(), static_cast<int>(buf.size()) - 1) == 0) {
        return "<unknown>";
    }
    return std::string{buf.data()};
}

/// @brief SendInput 落点守卫结果（`clear` 为 false 时 `reason` 说明点击为何不会落到目标）。
struct LandingCheck {
    bool clear = false;
    Point screen_px;  ///< 客户区点换算出的屏幕坐标（`ClientToScreen` 成功即可用，含拒绝态）
    std::string reason;
};

/// @brief SendInput 落点守卫：先把目标窗口抬到顶层/前景，再复核该客户区物理点对应的
///        屏幕点命中的顶层窗口仍是目标窗口。
///
/// SendInput 是全局注入、投给**光标所在窗口**：桌面被其他程序占用时（用户正在操作、
/// 前台锁定使夺焦被拒）同一次点击会落到另一窗口上，注入语义不成立——必须带原因拒绝，
/// 不得静默误投后让用例把环境竞争误判成管线故障（`post_click` 不经光标系统，可独立取证）。
/// 抬升是 best-effort（`BringWindowToTop` → `SetForegroundWindow`，被盖时再升 `TOPMOST`）；
/// 落点复核用 `WindowFromPoint`（按屏幕点查窗口，不依赖光标当前位置），命中的可能是目标的
/// 子窗口，故按**根窗口**比对。拒绝原因区分「被另一窗口盖住」与「落点落在目标矩形之外」——
/// 后者是坐标换算故障而非环境问题，须顺原因去查坐标而非当作环境跳过。
/// @param hwnd      目标窗口原生句柄（`Surface::native_handle()`）
/// @param client_px 客户区物理像素坐标（dp × `scale_factor`）
[[nodiscard]] inline auto sendinput_landing_check(void *hwnd, Point client_px) -> LandingCheck {
    const HWND target = static_cast<HWND>(hwnd);
    if (target == nullptr) {
        return LandingCheck{
            .clear = false,
            .reason = "null native handle (backend does not expose an HWND; SendInput has no coordinate source)"};
    }
    if (IsWindowVisible(target) == FALSE) {
        return LandingCheck{.clear = false,
                            .reason =
                                "target window is not visible (OS-level input needs the Normal tier: SendInput "
                                "hits the window under the cursor, a hidden window is never on that path)"};
    }
    // 抬升与夺焦皆 best-effort：失败不在此处裁决，由随后的落点复核决定。
    BringWindowToTop(target);
    if (GetForegroundWindow() != target) {
        SetForegroundWindow(target);
    }
    POINT pt{.x = static_cast<LONG>(client_px.x), .y = static_cast<LONG>(client_px.y)};
    if (ClientToScreen(target, &pt) == 0) {
        return LandingCheck{.clear = false, .reason = "ClientToScreen failed (window may already be gone)"};
    }
    if (GetAncestor(WindowFromPoint(pt), GA_ROOT) != target) {
        // 普通 z-order 抬升被盖：前台锁定限制的是「夺焦」，不限制自有窗口的 z-order 样式，
        // 故升级为 TOPMOST。会话结束窗口即销毁，无需（也无从）复原调用方的顶层状态。
        SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    const HWND root_under = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (root_under != target) {
        RECT rc{};
        GetWindowRect(target, &rc);
        // 落点在目标矩形之内 = 被别的窗口压住（环境竞争，用例侧记 skip）；矩形之外 = 坐标
        // 换算本身错了（真故障，不得当环境问题跳过）。
        const bool inside_target = (pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom);
        const std::string point = std::to_string(pt.x) + "," + std::to_string(pt.y);
        const std::string why = inside_target
                                    ? "landing point (" + point + ") is covered by " + window_class_of(root_under) +
                                          " even after the raise; SendInput would be delivered elsewhere"
                                    : "landing point (" + point + ") falls outside the target rect [" +
                                          std::to_string(rc.left) + "," + std::to_string(rc.top) + "," +
                                          std::to_string(rc.right) + "," + std::to_string(rc.bottom) +
                                          "]; coordinate accounting is broken, not an environment issue";
        return LandingCheck{.clear = false,
                            .screen_px = Point{.x = static_cast<float>(pt.x), .y = static_cast<float>(pt.y)},
                            .reason = why};
    }
    return LandingCheck{
        .clear = true, .screen_px = Point{.x = static_cast<float>(pt.x), .y = static_cast<float>(pt.y)}, .reason = ""};
}

/// @brief SendInput 全局注入：移动真实光标到目标窗口客户区物理像素坐标并左键点击。
///
/// 投递给**光标所在窗口**（点击同时会激活该窗口，随后可接键盘注入）；目标窗口须为
/// `Normal` 可见档。这是最接近真人操作的全链路通道（OS 输入流 → 命中窗口 → 窗口过程）。
/// 注入前先过 `sendinput_landing_check`（抬升 + 落点复核），落点不在目标上即带原因拒绝。
/// @param hwnd      目标窗口原生句柄（`Surface::native_handle()`，仅用于 client→screen 换算）
/// @param client_px 客户区物理像素坐标（dp × `scale_factor`，与宿主 on_mouse 的 px/scale 互逆）
[[nodiscard]] inline auto sendinput_click(void *hwnd, Point client_px) -> Result<void> {
    if (hwnd == nullptr) {  // 句柄缺失是用例/后端故障，不走守卫的「环境不可用」口径
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "sendinput_click: null native handle (backend does not expose an HWND)")};
    }
    const LandingCheck landing = sendinput_landing_check(hwnd, client_px);
    if (!landing.clear) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "sendinput_click: " + landing.reason)};
    }
    POINT pt{.x = static_cast<LONG>(landing.screen_px.x), .y = static_cast<LONG>(landing.screen_px.y)};
    if (SetCursorPos(pt.x, pt.y) == 0) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "sendinput_click: SetCursorPos failed")};
    }
    // INPUT 按 Win32 ABI 即联合体（type 判别 + mi/ki/hid 三选一），联合访问不可避免。
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;  // NOLINT(cppcoreguidelines-pro-type-union-access)
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;  // NOLINT(cppcoreguidelines-pro-type-union-access)
    if (SendInput(1U, &down, sizeof(INPUT)) != 1U) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported,
                                       "sendinput_click: SendInput(down) blocked (input stream unavailable?)")};
    }
    Sleep(20);  // 近似真实按压时长；注入流保序，松开必在按下之后
    if (SendInput(1U, &up, sizeof(INPUT)) != 1U) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported,
                                       "sendinput_click: SendInput(up) blocked (input stream unavailable?)")};
    }
    return Result<void>{};
}

/// @brief PostMessage 定向投递：不经光标/焦点系统，把左键按下/抬起直达窗口过程。
///
/// 覆盖的是「窗口过程 → Aurora 事件管线」这后半段（投递侧绕过 OS 输入流）；与前者的
/// 差异即通道价值：即便光标被占用、窗口非前台，管线接线仍可单独取证。
[[nodiscard]] inline auto post_click(void *hwnd, Point client_px) -> Result<void> {
    const HWND target = static_cast<HWND>(hwnd);
    if (target == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "post_click: null native handle (backend does not expose an HWND)")};
    }
    const auto lp = MAKELPARAM(static_cast<SHORT>(client_px.x), static_cast<SHORT>(client_px.y));
    if (PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp) == 0) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "post_click: PostMessage(WM_LBUTTONDOWN) failed")};
    }
    if (PostMessageW(target, WM_LBUTTONUP, 0, lp) == 0) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "post_click: PostMessage(WM_LBUTTONUP) failed")};
    }
    return Result<void>{};
}

/// @brief PostMessage 定向投递文本：逐字符 `WM_CHAR`（ASCII；与 win32_ime_live 探针同款）。
///
/// 经窗口过程的 `WM_CHAR` 处理 → `TextInputEvent` → 控件上屏，证明字符通道接线。
/// 仅支持 ASCII（多字节字符经 IME 通道，属 `--interactive` 探针的取证面）。
[[nodiscard]] inline auto post_text(void *hwnd, std::string_view text) -> Result<void> {
    const HWND target = static_cast<HWND>(hwnd);
    if (target == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "post_text: null native handle (backend does not expose an HWND)")};
    }
    for (const char c : text) {
        const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        if (PostMessageW(target, WM_CHAR, static_cast<WPARAM>(code), 0) == 0) {
            return Result<void>{make_error(ErrorCode::GeneralNotSupported, "post_text: PostMessage(WM_CHAR) failed")};
        }
    }
    return Result<void>{};
}

#endif  // AURORA_PLATFORM_WINDOWS

// ============================================================
// X11 通道（XTest：经 X 服务器合成真实输入事件）
// ------------------------------------------------------------
// dlopen libX11 + libXtst 免构建依赖（与 `aurora_verify_x11_ime_live` 探针同款）：
// 头文件不引入 X11 头/链接面，运行期符号缺失以 `xtest_available()` 如实上报。
// display / xid 取自 `X11Surface::native_display()` / `native_handle()`（同源连接，
// 事件直接进入本进程窗口的消息路径）。
// ============================================================
#if defined(AURORA_PLATFORM_UNIX) && !defined(AURORA_PLATFORM_MACOS)

#include <dlfcn.h>

namespace detail {

struct XDisplay;  // Xlib 不透明类型（自声明，免 X11 头）
using XWindow = unsigned long;  // XID
using XBool = int;

using TranslateFn = XBool (*)(XDisplay *, XWindow, XWindow, int, int, int *, int *, XWindow *);
using DefaultScreenFn = int (*)(XDisplay *);
using RootWindowFn = XWindow (*)(XDisplay *, int);
using KeysymToKeycodeFn = int (*)(XDisplay *, unsigned long);
using FlushFn = int (*)(XDisplay *);
using SetInputFocusFn = int (*)(XDisplay *, XWindow, int, unsigned long);
using FakeMotionFn = int (*)(XDisplay *, int, int, int, unsigned long);
using FakeButtonFn = int (*)(XDisplay *, unsigned int, XBool, unsigned long);
using FakeKeyFn = int (*)(XDisplay *, unsigned int, XBool, unsigned long);

/// @brief 本进程用到的 X11/XTest 符号集（dlopen 一次，进程生命周期持有，不 dlclose）。
struct X11Api {
    void *x11 = nullptr;
    void *xtst = nullptr;
    TranslateFn translate = nullptr;
    DefaultScreenFn default_screen = nullptr;
    RootWindowFn root_window = nullptr;
    KeysymToKeycodeFn keysym_to_keycode = nullptr;
    FlushFn flush = nullptr;
    SetInputFocusFn set_input_focus = nullptr;
    FakeMotionFn fake_motion = nullptr;
    FakeButtonFn fake_button = nullptr;
    FakeKeyFn fake_key = nullptr;

    [[nodiscard]] auto ok() const -> bool {
        return x11 != nullptr && xtst != nullptr && translate != nullptr && default_screen != nullptr &&
               root_window != nullptr && keysym_to_keycode != nullptr && flush != nullptr &&
               set_input_focus != nullptr && fake_motion != nullptr && fake_button != nullptr && fake_key != nullptr;
    }
};

/// @brief dlopen 并解析符号（缺任一库/符号即不 ok；调用方以明确原因跳过）。
[[nodiscard]] inline auto load_x11_api() -> X11Api {
    X11Api api;
    api.x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (api.x11 == nullptr) {
        api.x11 = dlopen("libX11.so", RTLD_NOW | RTLD_GLOBAL);
    }
    api.xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (api.xtst == nullptr) {
        api.xtst = dlopen("libXtst.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (api.x11 == nullptr || api.xtst == nullptr) {
        return X11Api{};
    }
    // NOLINTBEGIN(*-pro-type-reinterpret-cast)
    api.translate = reinterpret_cast<TranslateFn>(dlsym(api.x11, "XTranslateCoordinates"));
    api.default_screen = reinterpret_cast<DefaultScreenFn>(dlsym(api.x11, "XDefaultScreen"));
    api.root_window = reinterpret_cast<RootWindowFn>(dlsym(api.x11, "XRootWindow"));
    api.keysym_to_keycode = reinterpret_cast<KeysymToKeycodeFn>(dlsym(api.x11, "XKeysymToKeycode"));
    api.flush = reinterpret_cast<FlushFn>(dlsym(api.x11, "XFlush"));
    api.set_input_focus = reinterpret_cast<SetInputFocusFn>(dlsym(api.x11, "XSetInputFocus"));
    api.fake_motion = reinterpret_cast<FakeMotionFn>(dlsym(api.xtst, "XTestFakeMotionEvent"));
    api.fake_button = reinterpret_cast<FakeButtonFn>(dlsym(api.xtst, "XTestFakeButtonEvent"));
    api.fake_key = reinterpret_cast<FakeKeyFn>(dlsym(api.xtst, "XTestFakeKeyEvent"));
    // NOLINTEND(*-pro-type-reinterpret-cast)
    if (!api.ok()) {
        return X11Api{};
    }
    return api;
}

/// @brief X11/XTest 符号集（首次调用加载并缓存）。
[[nodiscard]] inline auto x11_api() -> const X11Api & {
    static const X11Api API = load_x11_api();
    return API;
}

}  // namespace detail

/// @brief XTest 通道是否可用（libX11/libXtst 可 dlopen 且符号齐全）。
[[nodiscard]] inline auto xtest_available() -> bool { return detail::x11_api().ok(); }

/// @brief XTest 注入左键点击：先把指针移到目标（窗口本地物理像素 → 根坐标），再按下/抬起。
///
/// 事件经 X 服务器命中测试后投给光标所在窗口——与真实鼠标完全同径。
/// @param display  `X11Surface::native_display()`（本进程窗口的 X 连接）
/// @param xid      `X11Surface::native_handle()`（窗口 XID）
/// @param local_px 窗口本地物理像素坐标（dp × `scale_factor`）
[[nodiscard]] inline auto xtest_click(void *display, void *xid, Point local_px) -> Result<void> {
    const detail::X11Api &api = detail::x11_api();
    if (!api.ok() || display == nullptr || xid == nullptr) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported,
                       "xtest_click: XTEST unavailable (libX11/libXtst dlopen failed) or null handles")};
    }
    auto *dpy = static_cast<detail::XDisplay *>(display);
    // XID 经 native_handle() 契约以全宽装入 void*，取回整数必须 reinterpret_cast（XWindow 为 unsigned long）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto win = static_cast<detail::XWindow>(reinterpret_cast<std::uintptr_t>(xid));
    int root_x = 0;
    int root_y = 0;
    detail::XWindow child = 0;
    if (api.translate(dpy, win, api.root_window(dpy, api.default_screen(dpy)), static_cast<int>(local_px.x),
                      static_cast<int>(local_px.y), &root_x, &root_y, &child) == 0) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument, "xtest_click: XTranslateCoordinates failed")};
    }
    api.fake_motion(dpy, api.default_screen(dpy), root_x, root_y, 0U);
    api.fake_button(dpy, 1U /*Button1*/, 1 /*press*/, 0U);
    api.fake_button(dpy, 1U /*Button1*/, 0 /*release*/, 0U);
    api.flush(dpy);
    return Result<void>{};
}

/// @brief 键盘焦点铺垫：`XSetInputFocus`（无窗口管理器时点击不会自动夺焦，与
///        `aurora_verify_x11_ime_live` 探针的焦点铺垫同款；等价 WM 的 click-to-focus）。
[[nodiscard]] inline auto xtest_focus_window(void *display, void *xid) -> Result<void> {
    const detail::X11Api &api = detail::x11_api();
    if (!api.ok() || display == nullptr || xid == nullptr) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "xtest_focus_window: XTEST unavailable or null handles")};
    }
    auto *dpy = static_cast<detail::XDisplay *>(display);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 同上，XID 装入 void* 契约的取回路径
    const auto win = static_cast<detail::XWindow>(reinterpret_cast<std::uintptr_t>(xid));
    api.set_input_focus(dpy, win, 1 /*RevertToParent*/, 0UL /*CurrentTime*/);
    api.flush(dpy);
    return Result<void>{};
}

/// @brief XTest 注入文本：逐字符 keysym→keycode 假键（ASCII 小写/数字；组合键不在本通道）。
[[nodiscard]] inline auto xtest_text(void *display, std::string_view text) -> Result<void> {
    const detail::X11Api &api = detail::x11_api();
    if (!api.ok() || display == nullptr) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "xtest_text: XTEST unavailable or null display")};
    }
    auto *dpy = static_cast<detail::XDisplay *>(display);
    for (const char c : text) {
        const auto keysym = static_cast<unsigned long>(static_cast<unsigned char>(c));
        const int keycode = api.keysym_to_keycode(dpy, keysym);
        if (keycode == 0) {
            return Result<void>{make_error(ErrorCode::GeneralInvalidArgument, "xtest_text: no keycode for ASCII char")};
        }
        api.fake_key(dpy, static_cast<unsigned int>(keycode), 1 /*press*/, 0U);
        api.fake_key(dpy, static_cast<unsigned int>(keycode), 0 /*release*/, 0U);
    }
    api.flush(dpy);
    return Result<void>{};
}

#endif  // X11 通道（UNIX 非 macOS）

// ============================================================
// macOS 通道（CGEvent：真实 OS 输入流，须辅助功能授权）
// ------------------------------------------------------------
// 与 Windows `SendInput` 同侧——全局注入、投给光标所在窗口，覆盖「OS 输入流 → 命中本窗口
// → 窗口过程 → Aurora 事件管线」这整段前半（X11 的 XTest 是同一形状）。
// 两条实测口径决定了本腿只能是「已授权的加强腿」，而不是默认证据：
//   - 未授权（`AXIsProcessTrusted()` 为假）时 `CGEventPost(kCGSessionEventTap, …)` 与
//     `CGEventPostToPid(getpid(), …)` **两条路一个事件都不送达**：AppKit 侧连
//     `addLocalMonitorForEventsMatchingMask:` 都收不到，控件状态与像素毫无变化；而投递侧
//     不报任何错（`CGEventPostToPid` 无返回值、`CGEventPost` 恒返回 success）。「什么都没
//     发生 + 没有错误」正是最容易被读成通过的形状，故未授权一律判通道不可用。
//   - 先把应用激活、把窗口做成 key（`activateIgnoringOtherApps:` + `makeKeyAndOrderFront:`）
//     再投，结论不变——不是命中测试打偏，是事件根本没进进程。
// 因此 macOS 的**默认证据**在 `tools/verify/macos_os_input_live_probe.mm`：同进程 AppKit
// 事件队列注入（`[NSApp postEvent:]` → `nextEventMatchingMask:` → `sendEvent:` → 内容视图 →
// `MacOSSurface` 翻译 → 派发）。该腿与 Win32 的 `post_click` 同侧：投递侧绕过 OS 输入流，
// 但管线后半段（含命中测试与手势状态机）单独取证得到。本头是纯 C++ 编译面，AppKit 消息
// 发送语法（ObjC++ + ARC）无法落在头里，故那一腿只在 `.mm` 探针取证——与 a11y 通道的
// 「协议面探针 + 已授权客户端加强腿」两段式完全同构。
// ============================================================
#ifdef AURORA_PLATFORM_MACOS

/// @brief CG 事件投递通道是否可用（= 本进程已持有辅助功能授权；未授权时投递静默无效）。
///
/// 授权无法在 SIP 环境里脚本授予（实测 `tccutil` 只能重置、不能写入；TCC 库在受保护卷上），
/// 故本机与 CI 上该函数为假，用例据此带原因记 SKIP。
[[nodiscard]] inline auto cg_input_channel_available() -> bool { return AXIsProcessTrusted() != 0; }

/// @brief 缺省显示面的高度（像素）。
///
/// E2E 侧的全局坐标一律**左上原点**（与 Win32 屏幕坐标、X11 根坐标同口径），CG 全局坐标是
/// **左下原点**：注入前须按本值翻转 y。返回 0 表示没有可用显示面（无头 / 无 Aqua 会话）。
[[nodiscard]] inline auto cg_display_height() -> double { return CGDisplayBounds(CGMainDisplayID()).size.height; }

/// @brief CGEvent 全局注入左键点击：把真实光标移到全局点，按下后原样抬起。
///
/// 光标真的会移动、点击真的会夺焦，故调用方须先把目标窗口抬升并聚焦（`Surface::raise()` +
/// `Surface::focus_window()`，对应 Win32 侧的 `BringWindowToTop` / `SetForegroundWindow`）——
/// 本腿**没有** Win32 那样的落点复核守卫：从纯 C++ 侧读不到 AppKit 的窗口框，
/// `CGWindowListCopyWindowInfo` 又需要屏幕录制授权（另一枚独立 TCC 权限）。因此该腿只在
/// 已授权、且调用方确认自己刚抬升过目标时成立；环境竞争（别的窗口压在落点上）会以「状态未
/// 翻转」的真实失败暴露，不静默跳过。
/// @param global_px 屏幕全局物理像素点（左上原点；由语义盒中心 × scale 加窗口原点得出）
[[nodiscard]] inline auto cg_click(Point global_px) -> Result<void> {
    if (!cg_input_channel_available()) {
        return Result<void>{make_error(
            ErrorCode::GeneralNotSupported,
            std::string{"cg_click: the accessibility grant is absent, so CGEvent posting is silently dropped "} +
                "(measured: neither kCGSessionEventTap nor CGEventPostToPid delivers a single event; "
                "default evidence lives in the aurora_verify_macos_os_input probe)")};
    }
    const double height = cg_display_height();
    if (height <= 0.0) {
        return Result<void>{
            make_error(ErrorCode::GeneralNotSupported, "cg_click: no display face to convert global coordinates on")};
    }
    const CGPoint point = CGPointMake(global_px.x, height - global_px.y);
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventRef move = CGEventCreateMouseEvent(source, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    CGEventRef down = CGEventCreateMouseEvent(source, kCGEventLeftMouseDown, point, kCGMouseButtonLeft);
    CGEventRef up = CGEventCreateMouseEvent(source, kCGEventLeftMouseUp, point, kCGMouseButtonLeft);
    if (move == nullptr || down == nullptr || up == nullptr) {
        if (move != nullptr) {
            CFRelease(move);
        }
        if (down != nullptr) {
            CFRelease(down);
        }
        if (up != nullptr) {
            CFRelease(up);
        }
        if (source != nullptr) {
            CFRelease(source);
        }
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "cg_click: CGEventCreateMouseEvent failed")};
    }
    // clickState 让按下/抬起配成同一次点击（缺省为 0 时 AppKit 会把它当孤立事件丢弃）。
    CGEventSetIntegerValueField(down, kCGMouseEventClickState, 1);
    CGEventSetIntegerValueField(up, kCGMouseEventClickState, 1);
    CGEventPost(kCGHIDEventTap, move);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(move);
    CFRelease(down);
    CFRelease(up);
    CFRelease(source);
    return Result<void>{};
}

/// @brief CGEvent 注入文本：逐字符建一对按下/抬起并挂上 Unicode 串（免键位表）。
///
/// `CGEventKeyboardSetUnicodeString` 让事件带出字符本身，AppKit 侧走 `keyDown:` 的
/// `characters:` 通道——与 Win32 的逐字符 `WM_CHAR`、X11 的 keysym 假键同侧。键码固定为 0：
/// 本腿取证的是**字符通道**接线，不是某个物理键的位置（键位映射属真人操作面）。
/// @param text 待注入文本（逐字节转 UTF-16 单元；非 ASCII 需调用方自行保证编码）
[[nodiscard]] inline auto cg_text(std::string_view text) -> Result<void> {
    if (!cg_input_channel_available()) {
        return Result<void>{make_error(
            ErrorCode::GeneralNotSupported,
            std::string{"cg_text: the accessibility grant is absent, so CGEvent posting is silently dropped"} +
                " (see cg_click for the measured shape)")};
    }
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    for (const char c : text) {
        const auto unit = static_cast<UniChar>(static_cast<unsigned char>(c));
        CGEventRef down = CGEventCreateKeyboardEvent(source, static_cast<CGKeyCode>(0U), true);
        CGEventRef up = CGEventCreateKeyboardEvent(source, static_cast<CGKeyCode>(0U), false);
        if (down == nullptr || up == nullptr) {
            if (down != nullptr) {
                CFRelease(down);
            }
            if (up != nullptr) {
                CFRelease(up);
            }
            if (source != nullptr) {
                CFRelease(source);
            }
            return Result<void>{
                make_error(ErrorCode::GeneralNotSupported, "cg_text: CGEventCreateKeyboardEvent failed")};
        }
        CGEventKeyboardSetUnicodeString(down, 1, &unit);
        CGEventKeyboardSetUnicodeString(up, 1, &unit);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
    }
    if (source != nullptr) {
        CFRelease(source);
    }
    return Result<void>{};
}

#endif  // macOS 通道（CGEvent）

}  // namespace aurora::e2e
