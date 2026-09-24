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
//
// 本头与 harness.h 同纪律：只依赖 aurora 公共头、不含测试框架宏；通道缺失（平台不支持、
// dlopen 失败）以 `Result` 错误或探测函数如实上报，**不是构建失败**。覆盖面与 a11y 通道
// 同口径：win32/d3d11（共用 Win32Host 输入管线，以 win32 为代表）与 x11；GLFW 不暴露
// 原生句柄（`native_handle()` 为基类空实现）、Wayland 无 XTest 等价物，均记为已知缺口。
// ============================================================

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
/// Windows：`OpenInputDesktop` 打不开输入桌面即非交互（锁定/安全桌面/服务会话）；
/// Linux：无 X 显示（`DISPLAY` 未设）即无输入面；macOS：通道未实现（已知缺口）。
[[nodiscard]] inline auto interactive_desktop() -> DesktopInteraction {
#ifdef AURORA_PLATFORM_WINDOWS
    // 需要真实交互桌面：能打开输入桌面才允许全局注入。
    const HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desk == nullptr) {
        return DesktopInteraction{
            .interactive = false,
            .reason = "cannot open the input desktop (locked screen / secure desktop / service session); "
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
#else
    return DesktopInteraction{.interactive = false,
                              .reason = "OS-level input channel not implemented on this platform (known gap)"};
#endif
}

// ============================================================
// Windows 通道（SendInput 全局注入 + PostMessage 定向投递）
// ============================================================
#ifdef AURORA_PLATFORM_WINDOWS

/// @brief SendInput 全局注入：移动真实光标到目标窗口客户区物理像素坐标并左键点击。
///
/// 投递给**光标所在窗口**（点击同时会激活该窗口，随后可接键盘注入）；目标窗口须为
/// `Normal` 可见档。这是最接近真人操作的全链路通道（OS 输入流 → 命中窗口 → 窗口过程）。
/// @param hwnd      目标窗口原生句柄（`Surface::native_handle()`，仅用于 client→screen 换算）
/// @param client_px 客户区物理像素坐标（dp × `scale_factor`，与宿主 on_mouse 的 px/scale 互逆）
[[nodiscard]] inline auto sendinput_click(void *hwnd, Point client_px) -> Result<void> {
    const HWND target = static_cast<HWND>(hwnd);
    if (target == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument,
                                       "sendinput_click: null native handle (backend does not expose an HWND)")};
    }
    POINT pt{.x = static_cast<LONG>(client_px.x), .y = static_cast<LONG>(client_px.y)};
    if (ClientToScreen(target, &pt) == 0) {
        return Result<void>{make_error(ErrorCode::GeneralInvalidArgument, "sendinput_click: ClientToScreen failed")};
    }
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
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "post_click: PostMessage(WM_LBUTTONDOWN) failed")};
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
[[nodiscard]] inline auto xtest_available() -> bool {
    return detail::x11_api().ok();
}

/// @brief XTest 注入左键点击：先把指针移到目标（窗口本地物理像素 → 根坐标），再按下/抬起。
///
/// 事件经 X 服务器命中测试后投给光标所在窗口——与真实鼠标完全同径。
/// @param display  `X11Surface::native_display()`（本进程窗口的 X 连接）
/// @param xid      `X11Surface::native_handle()`（窗口 XID）
/// @param local_px 窗口本地物理像素坐标（dp × `scale_factor`）
[[nodiscard]] inline auto xtest_click(void *display, void *xid, Point local_px) -> Result<void> {
    const detail::X11Api &api = detail::x11_api();
    if (!api.ok() || display == nullptr || xid == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported,
                                       "xtest_click: XTEST unavailable (libX11/libXtst dlopen failed) or null handles")};
    }
    auto *dpy = static_cast<detail::XDisplay *>(display);
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
        return Result<void>{make_error(ErrorCode::GeneralNotSupported,
                                       "xtest_focus_window: XTEST unavailable or null handles")};
    }
    auto *dpy = static_cast<detail::XDisplay *>(display);
    const auto win = static_cast<detail::XWindow>(reinterpret_cast<std::uintptr_t>(xid));
    api.set_input_focus(dpy, win, 1 /*RevertToParent*/, 0UL /*CurrentTime*/);
    api.flush(dpy);
    return Result<void>{};
}

/// @brief XTest 注入文本：逐字符 keysym→keycode 假键（ASCII 小写/数字；组合键不在本通道）。
[[nodiscard]] inline auto xtest_text(void *display, std::string_view text) -> Result<void> {
    const detail::X11Api &api = detail::x11_api();
    if (!api.ok() || display == nullptr) {
        return Result<void>{make_error(ErrorCode::GeneralNotSupported, "xtest_text: XTEST unavailable or null display")};
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

}  // namespace aurora::e2e
