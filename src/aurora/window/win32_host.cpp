#include "aurora/window/win32_host.h"

#include "aurora/window/detail/win32_ime.h"
#include "aurora/window/detail/win32_keymap.h"
#include "aurora/window/detail/win32_modifiers.h"
#include "aurora/window/detail/win32_ua.h"

#ifdef AURORA_BACKEND_WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "aurora/core/utf8.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"

namespace aurora {

// ---- 自由函数：修饰键 / 键码映射 / UTF-8 转换（不依赖实例，纯函数）----
// 派发期的修饰态一律读 `Impl::mods`（随消息流推进的跟踪器），**不再**逐事件异步采样——
// 那会让命中与否取决于「消息被泵到之前修饰键是否仍按着」，见 `detail/win32_modifiers.h`。
// 这里只剩一处合法的异步读数：窗口**获得激活**时取一次物理态基线，用于播种跟踪器
// （覆盖「用户 Alt+Tab 切进来、Alt 在激活之前就已按下」这种跟踪器无从得知的前置态）。
[[nodiscard]] static auto is_async_key_down(int vk) -> bool {
    // GetAsyncKeyState 返回有符号 SHORT；对最高位做位与时应先转无符号，
    // 避免 signed-bitwise 静态检查告警。
    return (static_cast<std::uint16_t>(GetAsyncKeyState(vk)) & 0x8000U) != 0U;
}

[[nodiscard]] static auto async_modifiers() -> ModifierKey {
    auto m = ModifierKey::None;
    if (is_async_key_down(VK_SHIFT)) {
        m = m | ModifierKey::Shift;
    }
    if (is_async_key_down(VK_CONTROL)) {
        m = m | ModifierKey::Control;
    }
    if (is_async_key_down(VK_MENU)) {
        m = m | ModifierKey::Alt;
    }
    if (is_async_key_down(VK_LWIN) || is_async_key_down(VK_RWIN)) {
        m = m | ModifierKey::Meta;
    }
    // NumLock 是切换键、不是瞬时按住态，故读它的**锁定指示**（toggle 位，bit 0）而不是
    // 「是否按住」。它是键盘上唯一的 NumLock 键，不存在左右之分，故无需归并。
    // 读不到（低版本 Windows 的兼容路径）时按「关」处理——不静默假报「开」。
    if ((static_cast<std::uint8_t>(GetKeyState(VK_NUMLOCK)) & 0x01U) != 0U) {
        m = m | ModifierKey::NumLock;
    }
    return m;
}

[[nodiscard]] static auto utf8_to_acp(const std::string &utf8) -> std::string {
    if (utf8.empty()) {
        return std::string{};
    }
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) {
        return utf8;
    }
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), w.data(), wn);
    const int an = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
    if (an <= 0) {
        return utf8;
    }
    std::string a(static_cast<size_t>(an), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), wn, a.data(), an, nullptr, nullptr);
    return a;
}

// ---- 几何态/可见性态分类（创建分族与尺寸分族共用）----
[[nodiscard]] static auto classify_size_mode(WPARAM wp) -> WindowMode {
    switch (wp) {
        case SIZE_MINIMIZED:
            return WindowMode::Minimized;
        case SIZE_MAXIMIZED:
            return WindowMode::Maximized;
        case SIZE_RESTORED:  // NOLINT(*-branch-clone)
            return WindowMode::Normal;
        default:
            return WindowMode::Normal;  // SIZE_MAXHIDE / SIZE_MAXSHOW：不改变几何态
    }
}

// pimpl：全部 Win32/GDI 状态与消息处理都在这里；公共头仅持有 unique_ptr<Impl>。
struct Win32Host::Impl {
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    Size size{.width = 0.0F, .height = 0.0F};
    /// device pixel ratio（dp ↔ 物理像素）。**本成员是宿主内 dp↔物理换算的唯一真值源**：
    /// 句柄就绪后由 `refresh_scale()` 按 `GetDpiForWindow(hwnd)` 求得（`WM_DPICHANGED` 与跨屏
    /// 迁移时更新），其余一律读它——不得再调任何「现调 DPI」的函数，否则逻辑尺寸与上报给
    /// 消费方的 `scale` 会各走一条路径而发散（历史缺口见 `specification/08-tooling.md` §8.2）。
    float scale = 1.0F;
    WindowStyleOptions style{};  ///< 高级样式（置顶/无边框/尺寸限制）。
    bool should_close = false;
    EventHandler handler;
    int present_count = 0;  ///< 已触发同步重渲染次数（WM_SIZE/WM_PAINT 计数）。
    bool minimized = false;  ///< 是否最小化（WM_SIZE SIZE_MINIMIZED）。
    bool tracking_leave = false;  ///< 是否已登记 WM_MOUSELEAVE 通知（TrackMouseEvent 一次性，hover 清除用）。
    bool active = true;  ///< 是否前台激活（WM_ACTIVATE 非 WA_INACTIVE）。初值 true：创建即激活。
    bool maximized = false;  ///< 是否最大化（WM_SIZE SIZE_MAXIMIZED）。
    WindowMode mode = WindowMode::Normal;  ///< 当前几何态（状态变化时上报）。
    WindowState state = WindowState::Visible;  ///< 当前可见性状态（变化时上报）。
    WindowStateHandler window_state_handler;
    WindowModeHandler window_mode_handler;
    PresentRequest present_request;
    std::function<void(float)> scale_handler;  ///< DPI 缩放变化上报（`WM_DPICHANGED` 后触发）。
    /// @brief 无障碍桥（由窗口宿主持有，GDI / D3D11 两个 Surface 共用同一实例）。
    std::unique_ptr<detail::Win32UiaBridge> a11y;
    /// @brief 宿主接管 `WM_GETOBJECT` 的钩子（默认空 → 走内置桥）。
    std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> a11y_hook;
    /// @brief 最近一次注入的语义树根（非拥有；桥尚未构造时先由宿主记下）。
    Widget *a11y_root = nullptr;

    /// @brief IMM32 组合输入桥（与窗口同生命周期；无输入法激活时零消息、零成本）。
    std::unique_ptr<detail::Win32ImeBridge> ime;
    /// @brief 修饰键跟踪器：随 `WM_KEY*` / `WM_SYSKEY*` 推进，失焦清空、重新激活播种。
    detail::ModifierKeyTracker mods;
    /// @brief 焦点控件的候选窗定位盒查询（由 `WindowHost` 注入；空 = 无定位，走系统默认）。
    std::function<Rect()> composition_caret_provider;

    inline static bool class_registered = false;
    inline static HBRUSH bg_brush = nullptr;  ///< 浅色背景擦除刷（消除最大化黑屏），注册时创建一次。
    static constexpr auto AURORA_CLASS_NAME = "AuroraWin32Surface";

    Impl(int w, int h, const std::string &title, const WindowStyleOptions &style,
         WindowVisibility visibility = WindowVisibility::Normal);
    ~Impl();
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    // 事件翻译（输入分族内部调用）
    auto on_mouse(MouseAction action, MouseButton button, int x, int y) const -> void;
    auto on_wheel(int delta, int x, int y) const -> void;
    /// @brief 翻译一条键盘消息并派发；`WM_SYSKEY*` 依此判定是否回落 `DefWindowProcA`。
    /// @return 该 `KeyEvent` 是否被消费（`is_handled`）；无 handler 时恒 false（不消费）。
    [[nodiscard]] auto on_key(KeyAction action, int vk, LPARAM lp) const -> bool;
    auto on_char(std::uint32_t ch) const -> void;

    /// @brief 由最小化/激活标志重算可见性状态，仅实际改变时上报（避免重复通知）。
    auto update_window_state() -> void;

    // ---- 消息分族处理函数（wnd_proc 按消息族分发到此处）----
    static auto handle_create() -> LRESULT;
    auto handle_mouse(HWND hwnd_in, UINT msg, LPARAM lp) -> LRESULT;
    auto handle_wheel(HWND hwnd_in, WPARAM wp, LPARAM lp) const -> LRESULT;
    [[nodiscard]] auto handle_key(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
    [[nodiscard]] auto handle_syskey(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
    [[nodiscard]] auto handle_char(WPARAM wp) const -> LRESULT;
    auto handle_size(HWND hwnd_in, WPARAM wp, LPARAM lp) -> LRESULT;
    auto handle_paint(HWND hwnd_in) -> LRESULT;
    auto handle_activate(WPARAM wp) -> LRESULT;
    /// @brief DPI 变化：先按系统给出的新矩形就位，再按 wParam 的新 DPI 更新缩放并上报。
    /// 非 static：`WM_DPICHANGED` 需要回写本窗口的 `scale` 并通知其 handler。
    auto handle_dpi_changed(HWND hwnd, WPARAM wp, LPARAM lp) -> LRESULT;
    /// @brief 上报当前 DPI 缩放（由 `handle_dpi_changed` 调用）。
    auto notify_scale_changed() const -> void;
    [[nodiscard]] auto handle_getminmaxinfo(LPARAM lp) const -> LRESULT;
    auto handle_close() -> LRESULT;
    auto handle_destroy() -> LRESULT;
    /// @brief `WM_GETOBJECT`：仅应答 UIA 根请求（`UiaRootObjectId`），其余交 `DefWindowProc`
    ///        （MSAA / 系统代理兜底，非目标）。返回 nullopt = 未处理。
    [[nodiscard]] auto handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT>;
    /// @brief `WM_IME_*` 分族：交 IMM32 桥翻译组合；桥不认领（含 `WM_KILLFOCUS` 的取消侧路径）
    ///        时回落 `DefWindowProc`。
    [[nodiscard]] auto handle_ime(UINT msg, WPARAM wp, LPARAM lp) const -> LRESULT;
    auto handle_dropfiles(HWND hwnd_in, LPARAM lp) const -> LRESULT;

    auto register_class() const -> void;
    /// @brief 按当前窗口重算 `scale`（唯一读 DPI 处）；句柄未就绪时按 96 处理。
    auto refresh_scale() -> void;
    /// @brief 物理像素 → 逻辑 dp。宿主内唯一的「除 scale」入口。
    [[nodiscard]] auto to_logical(int px, int py) const -> Point;
    /// @brief 逻辑 dp → 物理像素。宿主内唯一的「乘 scale」入口。
    [[nodiscard]] auto to_physical(Size dp) const -> Size;

    // 窗口过程：仅在最早时机（WM_NCCREATE/WM_CREATE）把 Impl* 存入 GWLP_USERDATA，
    // 随后按消息族分发到对应 handle_* 处理函数（创建/输入/尺寸/绘制/关闭 等）。
    static auto WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
};

// ---- Impl 构造：窗口创建 + DPI 适配 + 类注册 + 显示 ----
Win32Host::Impl::Impl(int w, int h, const std::string &title, const WindowStyleOptions &style,
                      WindowVisibility visibility)
    : style(style) {
    // DPI 感知必须早于**任何**窗口创建：`create_window` 路径已由 `make_window` 前移启用；
    // 直接构造 `Win32Host` 的消费者（不经工厂）由这处兜底。函数幂等（进程级 static 守卫），
    // 两条路径都过不会重复设置。`scale` 此刻**不**取值——此时 hwnd 仍是 nullptr，进程感知未必
    // 生效，`GetDeviceCaps` 只会回 96（scale 恒 1.0，正是历史缺口）；它在建窗后由
    // `refresh_scale()` 求得。
    enable_dpi_awareness();

    // 适配工作区，保证窗口在屏幕内可见（逻辑 dp）。
    RECT wa{};
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0) != 0) {
        const int max_w = wa.right - wa.left;
        const int max_h = wa.bottom - wa.top;
        w = std::min(w, max_w);
        h = std::min(h, max_h);
    }
    if (w <= 0) {
        w = 320;
    }
    if (h <= 0) {
        h = 240;
    }

    hinst = GetModuleHandleA(nullptr);
    register_class();

    // 高级样式映射：无边框 WS_POPUP；不可调大小去 WS_THICKFRAME/WS_MAXIMIZEBOX。
    DWORD win_style = WS_OVERLAPPEDWINDOW;
    if (style.frameless) {
        win_style = WS_POPUP;
    } else if (!style.resizable) {
        win_style = static_cast<DWORD>(WS_OVERLAPPEDWINDOW) &
                    ~(static_cast<DWORD>(WS_THICKFRAME) | static_cast<DWORD>(WS_MAXIMIZEBOX));
    }
    const DWORD ex_style = (style.always_on_top ? WS_EX_TOPMOST : 0U) | (style.transparent ? WS_EX_LAYERED : 0U);

    // 句柄尚未创建，但 DPI 感知此时已生效（见构造体开头的 `enable_dpi_awareness`），故先取一次
    // 显示器 DPI 作为建窗尺寸的换算基准；建窗后立刻由 `refresh_scale()` 换成按窗口的真实值。
    refresh_scale();

    // DPI 感知下窗口坐标即物理像素：物理窗口尺寸 = 逻辑 dp × scale。
    const Size logical_size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    const Size physical_size = to_physical(logical_size);
    RECT rect{.left = 0,
              .top = 0,
              .right = static_cast<int>(std::lround(physical_size.width)),
              .bottom = static_cast<int>(std::lround(physical_size.height))};
    AdjustWindowRect(&rect, win_style, FALSE);
    const int win_w = rect.right - rect.left;
    const int win_h = rect.bottom - rect.top;

    // 标题为 UTF-8；CreateWindowExA 按进程 ANSI 代码页解释字节，故先转 UTF-8 → ACP 再传 ANSI API。
    const std::string ansi_title = utf8_to_acp(title);
    // 把 Impl* 作为 lpParam 传入，WM_NCCREATE 时存入 GWLP_USERDATA（wnd_proc 取回）。
    hwnd = CreateWindowExA(ex_style, AURORA_CLASS_NAME, ansi_title.c_str(), win_style, CW_USEDEFAULT, CW_USEDEFAULT,
                           win_w, win_h, nullptr, nullptr, hinst, this);

    if (hwnd != nullptr) {
        // 句柄就绪：换成**按窗口**的真实 DPI（跨显示器时与所在屏一致），此后 `scale` 全程只读
        // 这一个成员。这是「帧 / 逻辑 / scale 三方同源」的起点。
        refresh_scale();
        if (style.always_on_top) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        // 可见性策略：构造期一次定档，避免「先可见后隐藏」造成的一帧闪烁。
        // 窗口本就以无 WS_VISIBLE 的 WS_OVERLAPPEDWINDOW 创建，Hidden 档显式隐藏以固化语义。
        switch (visibility) {
            case WindowVisibility::NoActivate:
                ShowWindow(hwnd, SW_SHOWNA);  // 显示但不激活：不抢焦点
                break;
            case WindowVisibility::Hidden:
                ShowWindow(hwnd, SW_HIDE);  // 不显示：窗口不进入用户视野
                break;
            case WindowVisibility::Normal:
                ShowWindow(hwnd, SW_SHOW);
                break;
        }
        UpdateWindow(hwnd);
        DragAcceptFiles(hwnd, TRUE);  // 启用操作系统文件拖放（WM_DROPFILES）
        // IMM32 组合桥：桥自身只吃 WM_IME_*，无输入法时一条也不来，故随窗口直接构造。
        ime = std::make_unique<detail::Win32ImeBridge>(
            hwnd, detail::Win32ImeBridge::Hooks{.emit = [this](Event &e) -> void {
                                                    if (handler) {
                                                        handler(e);
                                                    }
                                                },
                                                .caret_bounds = [this]() -> Rect {
                                                    return composition_caret_provider ? composition_caret_provider()
                                                                                      : Rect{};
                                                },
                                                .scale_factor = [this]() -> float { return scale; }});
    }
    size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};  // 逻辑 dp（布局用）
}

Win32Host::Impl::~Impl() {
    // 先断开全部上层回调再销毁窗口：DestroyWindow 会**同步**派发 WM_ACTIVATE/WM_KILLFOCUS/
    // WM_MOUSELEAVE 等消息，而此刻 Window 的其他成员（dirty_ 等，按声明逆序已先于
    // surface_ 析构）与上层 Application 状态可能已亡——回调链（事件派发 → hover 清除 →
    // mark_needs_paint → wire_dirty 捕获的 Window*）会触发 use-after-free
    // （关闭窗口时 0xC0000005，-O0/coverage 构建下稳定复现）。
    handler = nullptr;
    window_state_handler = nullptr;
    window_mode_handler = nullptr;
    present_request = nullptr;
    composition_caret_provider = nullptr;
    ime = nullptr;  // 桥的 hooks 捕获本 Impl，须在 DestroyWindow 前先散
    if (hwnd != nullptr) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
        // 排干残留 WM_QUIT（历史防御）：本后端已不再投递 WM_QUIT（见 handle_destroy），
        // 但若宿主代码/系统经 PostQuitMessage 主动请求退出，残留的 WM_QUIT 仍会让同线程
        // 后续新建窗口的消息循环首次 poll 就误判 should_close（顺序创建多窗口时立即退出）。
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != 0) {
        }
    }
}

auto Win32Host::Impl::on_mouse(MouseAction action, MouseButton button, int x, int y) const -> void {
    if (!handler) {
        return;
    }
    // 指针捕获：按下即把整个窗口捕获，使拖拽/拖选时光标移出窗口仍能收到 Move/Release。
    if (action == MouseAction::Press) {
        ::SetCapture(hwnd);
    } else if (action == MouseAction::Release) {
        ::ReleaseCapture();
    }
    MouseEvent e;
    e.action = action;
    e.button = button;
    e.position = to_logical(x, y);
    handler(e);
}

auto Win32Host::Impl::on_wheel(int delta, int x, int y) const -> void {
    if (!handler) {
        return;
    }
    ScrollEvent e;
    e.position = to_logical(x, y);
    e.delta_y = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
    handler(e);
}

[[nodiscard]] auto Win32Host::Impl::on_key(KeyAction action, int vk, LPARAM lp) const -> bool {
    if (!handler) {
        return false;
    }
    KeyEvent e;
    e.action = action;
    // 导航区在 Win32 上与小键盘共用 VK，来处只在 lParam 的扫描码里，且六个键里只有 Home
    // 真的可分（详见 `is_numpad_nav_scan` 的实测对照表），故映射带这个判据。
    const bool from_numpad = detail::is_numpad_nav_scan(vk, detail::scan_code_of(lp));
    e.key = static_cast<int>(detail::from_win32_vk(vk, from_numpad));
    e.modifiers = mods.get();
    handler(e);
    return e.is_handled;
}

auto Win32Host::Impl::on_char(std::uint32_t ch) const -> void {
    if (!handler) {
        return;
    }
    if (ch < 0x20) {
        return;  // 控制字符交给 KeyEvent 处理。
    }
    TextInputEvent e;
    e.text = utf8_encode(ch);
    handler(e);
}

auto Win32Host::Impl::update_window_state() -> void {
    const WindowState want = compute_window_state(minimized, active);
    if (want != state) {
        state = want;
        if (window_state_handler) {
            window_state_handler(want);
        }
    }
}

// ---- 创建分族 ----
auto Win32Host::Impl::handle_create() -> LRESULT { return 0; }

// ---- 输入分族（鼠标按钮 / 移动 / 离开 / 滚轮 / 键 / 字符）----
auto Win32Host::Impl::handle_mouse(HWND hwnd_in, UINT msg, LPARAM lp) -> LRESULT {
    switch (msg) {
        case WM_LBUTTONDOWN:
            on_mouse(MouseAction::Press, MouseButton::Left, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            on_mouse(MouseAction::Release, MouseButton::Left, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_RBUTTONDOWN:
            on_mouse(MouseAction::Press, MouseButton::Right, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_RBUTTONUP:
            on_mouse(MouseAction::Release, MouseButton::Right, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            // 悬停追踪（hover 基础设施）：首次 Move 时登记 WM_MOUSELEAVE 通知，
            // 光标离开窗口时能清除控件悬停态（否则 hover 高亮永久残留）。
            if (!tracking_leave) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_in;
                if (TrackMouseEvent(&tme) != 0) {
                    tracking_leave = true;
                }
            }
            on_mouse(MouseAction::Move, MouseButton::Left, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            // 光标离开客户区：合成一次远离窗口的 Move，使派发器命中空链 → 清除全部悬停态。
            tracking_leave = false;  // 下次 Move 重新登记（TME_LEAVE 为一次性通知）
            on_mouse(MouseAction::Move, MouseButton::Left, -10000, -10000);
            return 0;
        default: {
            return DefWindowProcA(hwnd_in, msg, 0, lp);
        }
    }
}

auto Win32Host::Impl::handle_wheel(HWND hwnd_in, WPARAM wp, LPARAM lp) const -> LRESULT {
    POINT pt{.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_in, &pt);
    on_wheel(GET_WHEEL_DELTA_WPARAM(wp), pt.x, pt.y);
    return 0;
}

auto Win32Host::Impl::handle_key(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    const auto action = (msg == WM_KEYUP) ? KeyAction::Up : KeyAction::Down;
    // 先推进修饰态再派发：本条消息若正是修饰键自身，它应当计入本事件的 `modifiers`
    // （Windows 的常规语义是「Ctrl 按下的那条 KeyEvent 就带 Control」，热键匹配依赖它）。
    mods.apply(static_cast<int>(wp), action == KeyAction::Down);
    // 常规键恒由 Aurora 消费：派发结果在此无关紧要（不走 DefWindowProcA），故显式弃置。
    (void)on_key(action, static_cast<int>(wp), lp);
    return 0;
}

// `WM_SYSKEY*` = 按住 Alt 期间的按键（Alt 自身也算）。与常规键**同路进派发链**：
// 先推进修饰态（与 `handle_key` 同口径，故「按下 Alt 的那条 KeyEvent 自身就带 Alt 位」），
// 再经同一个 `on_key` 通道产出 `KeyEvent`；被消费则 `return 0`（Aurora 侧认领），
// 未消费则原样回落 `DefWindowProcA`——`Alt+F4` 关闭、`Alt+Tab` 切换与菜单助记键都由系统
// 实现，只有在「无人认领」时才发生，不会被无条件掐死。
//
// 例外（`VK_MENU` 左右与 `VK_F10`）：只推进修饰态、不派发。它们分别是修饰键与系统菜单键，
// 发出的键码无语义且会污染紧随其后的快捷键匹配（见 `detail::syskey_dispatches`）。
// 文本通道不受影响：Alt 系本就不产 `TextInputEvent`（Windows 不为 Alt 组合发 `WM_CHAR`），
// 故不存在同一字符两条通道重复上屏。
auto Win32Host::Impl::handle_syskey(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    const int vk = static_cast<int>(wp);
    mods.apply(vk, msg != WM_SYSKEYUP);
    if (detail::syskey_dispatches(vk) && on_key(msg == WM_SYSKEYUP ? KeyAction::Up : KeyAction::Down, vk, lp)) {
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

auto Win32Host::Impl::handle_char(WPARAM wp) const -> LRESULT {
    // 组合期残余 WM_CHAR 整条丢弃：IME 上屏结果统一走 `GCS_RESULTSTR`（见 win32_ime 桥），
    // 两条通道并存即「同一汉字上屏两次」。
    if (ime != nullptr && ime->is_composing()) {
        return 0;
    }
    on_char(static_cast<std::uint32_t>(wp));
    return 0;
}

// ---- 尺寸分族（WM_SIZE）----
auto Win32Host::Impl::handle_size(HWND hwnd_in, WPARAM wp, LPARAM lp) -> LRESULT {
    // DPI 感知下 lParam 为物理像素；换算回逻辑 dp 供布局使用。
    const int pw = static_cast<int>(LOWORD(lp));
    const int ph = static_cast<int>(HIWORD(lp));
    if (pw > 0 && ph > 0) {
        // 读缓存的 `scale`（不现调 DPI）：这是「帧 / 逻辑 / scale 三方同源」的关键一处。
        // 现调 DPI 会与上报给消费方的 `scale` 各走一条路径，在 ≠100% DPI 显示器上发散。
        const Point logical = to_logical(pw, ph);
        size = Size{.width = logical.x, .height = logical.y};
    }
    // 几何态变化（最小化/最大化/还原）上报，仅模式实际改变时通知。
    const WindowMode want = classify_size_mode(wp);
    if (want != mode) {
        mode = want;
        minimized = (want == WindowMode::Minimized);
        maximized = (want == WindowMode::Maximized);
        if (window_mode_handler) {
            window_mode_handler(want);
        }
        update_window_state();
    }
    // 几何变化当下同步重渲染：让帧缓冲在 DWM 合成最大化/缩放动画前已为新尺寸内容。
    if (present_request) {
        present_request();
        ++present_count;  // 观测器：统计已触发同步重渲染次数
        // 同步重渲染已把新尺寸内容上屏：验证客户区，免去紧跟的 WM_PAINT 再整窗
        // 重推一次（最大化连发 WM_SIZE+WM_PAINT 时双重全量上屏是卡顿放大器）。
        ValidateRect(hwnd_in, nullptr);
    }
    return 0;
}

// ---- 绘制分族（WM_PAINT）----
auto Win32Host::Impl::handle_paint(HWND hwnd_in) -> LRESULT {
    // 最大化/缩放时 OS 要求重绘：立即把已就绪的帧缓冲呈现到窗口，填平空档，避免黑屏与旧内容残留。
    // 未被覆盖的扩展区域由浅色背景刷擦除（非纯黑）。present_request_ 即 Window 的同步重渲染
    // （present_root 内含 present），已把缓冲上屏；此处仅触发一次。
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_in, &ps);
    if (present_request) {
        present_request();  // 必要时按当前尺寸刷新帧缓冲并上屏
        ++present_count;
    }
    EndPaint(hwnd_in, &ps);
    return 0;
}

// ---- 激活分族（WM_ACTIVATE）----
auto Win32Host::Impl::handle_activate(WPARAM wp) -> LRESULT {
    const bool new_active = (LOWORD(wp) != WA_INACTIVE);
    if (new_active != active) {
        active = new_active;
        // 修饰态与前台状态同步：拿到前台时用物理读数播种一次（「Alt+Tab 切进来」的那条 Alt
        // 按下属于别的窗口，跟踪器无从得知）；交出前台时整体清空（未送达的抬起消息不可追，
        // 留着就是幻影位）。二者都只在状态真翻转时做一次，不是逐事件采样。
        if (new_active) {
            mods.seed(async_modifiers());
        } else {
            mods.clear();
        }
        update_window_state();
    }
    return 0;
}

// ---- DPI 变化分族（WM_DPICHANGED）----
auto Win32Host::Impl::handle_dpi_changed(HWND hwnd, [[maybe_unused]] WPARAM wp, LPARAM lp) -> LRESULT {
    const auto *pr = reinterpret_cast<RECT *>(lp);  // NOLINT(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    // 缩放更新与上报表层：由 `Window` 强制全量重排重绘（否则 logical↔physical 换算失配会内容
    // 错位 / 发虚）。取值走 `refresh_scale()`（`GetDpiForWindow`）而非 `wParam` 的 HIWORD——
    // 两者在正常路径上同值，但前者是**按窗口**的权威读数，与构造期、跨屏迁移共用同一条路径，
    // 不会因某次消息的 wParam 异常而与其它换算点脱钩。
    const float before = scale;
    refresh_scale();
    if (scale != before) {
        notify_scale_changed();
    }
    return 0;
}

auto Win32Host::Impl::notify_scale_changed() const -> void {
    if (scale_handler) {
        scale_handler(scale);
    }
}

// ---- 尺寸限制分族（WM_GETMINMAXINFO）----
auto Win32Host::Impl::handle_getminmaxinfo(LPARAM lp) const -> LRESULT {
    // 尺寸限制：逻辑 dp → 物理像素。与 `set_size` 走**同一个** `to_physical`，否则 150% 屏上
    // 「按 1.5 放大上报 → OS 最大化到物理 3840 → WM_SIZE 再除回 1.5」会产生往返漂移。
    auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);  // NOLINT(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    const WindowStyleOptions &st = style;
    const Size min_px = to_physical(st.min_size);
    const Size max_px = to_physical(st.max_size);
    if (st.min_size.width > 0.0F) {
        mmi->ptMinTrackSize.x = static_cast<LONG>(std::lround(min_px.width));
    }
    if (st.min_size.height > 0.0F) {
        mmi->ptMinTrackSize.y = static_cast<LONG>(std::lround(min_px.height));
    }
    if (st.max_size.width > 0.0F) {
        mmi->ptMaxTrackSize.x = static_cast<LONG>(std::lround(max_px.width));
    }
    if (st.max_size.height > 0.0F) {
        mmi->ptMaxTrackSize.y = static_cast<LONG>(std::lround(max_px.height));
    }
    return 0;
}

// ---- 关闭分族（WM_CLOSE / WM_DESTROY）----
auto Win32Host::Impl::handle_close() -> LRESULT {
    should_close = true;
    return 0;
}

auto Win32Host::Impl::handle_destroy() -> LRESULT {
    should_close = true;
    // 窗口销毁：先全量断连 UIA provider 再置空 hwnd —— 否则 UIA 侧缓存的 provider 会
    // 在窗口已销毁后回调进来（Chromium / Qt 已知崩溃源，R1）。
    if (a11y != nullptr) {
        a11y->disconnect_all();
        a11y.reset();
    }
    hwnd = nullptr;
    // 多窗口：**不再** `PostQuitMessage(0)`。
    // `WM_QUIT` 是**线程级**的：任一个窗口销毁都投递它，会让同线程内所有窗口
    // （乃至之后新建的窗口）在首次 poll 时被误判为「已请求关闭」——这正是
    // 「关掉一个窗口整个应用退出」以及「顺序创建多窗口时立即退出」的根因。
    // 现在关闭语义完全收敛到**本窗口**的 should_close，是否退出进程由
    // `Application` 的退出策略（`ExitPolicy`）决定。
    return 0;
}

// ---- 无障碍分族（WM_GETOBJECT → UIA 桥）----
auto Win32Host::Impl::handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT> {
    if (a11y_hook) {
        // 公共签名用指针宽度整数（避免公共头引入 <windows.h>），此处还原为原生类型。
        if (auto answered = a11y_hook(static_cast<std::uintptr_t>(wp), static_cast<std::intptr_t>(lp));
            answered.has_value()) {
            return static_cast<LRESULT>(*answered);  // 宿主接管
        }
    }
    constexpr LONG uia_root_object_id = -25;
    // UIA 根请求的 lParam = UiaRootObjectId(-25)：按 Win32 惯例两侧都截断到 DWORD 比较。
    // 不可用混合符号的安全比较（cmp_not_equal 按数学值判等，0xFFFFFFE7 与 -25 永不命中，
    // 曾致内置桥无法经 WM_GETOBJECT 激活）；UIA 协议本身就是 (DWORD)lParam == (DWORD)id。
    if (static_cast<DWORD>(lp) != static_cast<DWORD>(uia_root_object_id)) {
        return std::nullopt;  // 非 UIA 根请求：交 DefWindowProc（MSAA 兜底）
    }
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    if (a11y == nullptr) {
        // 惰性构造：无读屏在线时连桥对象都不存在 ⇒ 零开销。
        a11y = std::make_unique<detail::Win32UiaBridge>(hwnd);
        a11y->set_root(a11y_root);  // 补喂：宿主在桥存在前已记下的根
    }
    return a11y->handle_get_object(wp, lp);
}

// ---- 输入法分族（WM_IME_* → IMM32 组合桥）----
auto Win32Host::Impl::handle_ime(UINT msg, WPARAM wp, LPARAM lp) const -> LRESULT {
    if (ime != nullptr) {
        if (const std::optional<LRESULT> answered = ime->handle(msg, lp); answered.has_value()) {
            return *answered;
        }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

auto Win32Host::accessibility_provider() const -> a11y::Provider * { return pimpl_->a11y.get(); }

auto Win32Host::set_accessibility_hook(
    std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> h) const -> void {
    pimpl_->a11y_hook = std::move(h);
}

auto Win32Host::set_accessibility_root(Widget *root) const -> void {
    pimpl_->a11y_root = root;
    if (pimpl_->a11y != nullptr) {
        pimpl_->a11y->set_root(root);
    }
}

auto Win32Host::set_composition_caret_provider(std::function<Rect()> provider) const -> void {
    pimpl_->composition_caret_provider = std::move(provider);
}

auto Win32Host::ime_composing() const -> bool { return pimpl_->ime != nullptr && pimpl_->ime->is_composing(); }

// ---- 文件拖放分族（WM_DROPFILES）----
auto Win32Host::Impl::handle_dropfiles(HWND hwnd_in, LPARAM lp) const -> LRESULT {
    // 操作系统文件拖放：解析 HDROP 为 UTF-8 路径列表，落点换算为窗口逻辑坐标。
    auto *const hdrop = reinterpret_cast<HDROP>(lp);  // NOLINT(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring ws(static_cast<std::size_t>(len) + 1U, L'\0');
        DragQueryFileW(hdrop, i, ws.data(), len + 1);
        const int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string s(static_cast<std::size_t>(n) - 1U, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, s.data(), n, nullptr, nullptr);
            paths.push_back(std::move(s));
        }
    }
    POINT pt{};
    DragQueryPoint(hdrop, &pt);
    ScreenToClient(hwnd_in, &pt);  // 屏幕坐标 → 客户区坐标
    DragFinish(hdrop);
    FileDropEvent fde;
    fde.position = to_logical(pt.x, pt.y);
    fde.paths = std::move(paths);
    if (handler) {
        handler(fde);
    }
    return 0;
}

auto Win32Host::Impl::register_class() const -> void {
    if (class_registered) {
        return;
    }
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Impl::wnd_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = AURORA_CLASS_NAME;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (bg_brush == nullptr) {
        bg_brush = CreateSolidBrush(RGB(245, 245, 247));  // 浅色擦除刷，消除最大化黑屏
    }
    wc.hbrBackground = bg_brush;
    if (RegisterClassExA(&wc) != 0U) {
        class_registered = true;
    }
}

// ---- DPI：唯一真值源与两处换算 ----
//
// `refresh_scale()` 是**唯一**读 DPI 的地方：按窗口（而非按显示器 / 按 DC）取值，故跨屏迁移后
// 立即反映新屏 DPI。取不到时逐级回落 `GetDpiForSystem` → 96，任何一级取不到都按 96（= 1.0）
// 处理而不是猜。
auto Win32Host::Impl::refresh_scale() -> void {
    int dpi = 0;
    // GetDpiForWindow / GetDpiForSystem 只在 Win8.1+ / Win10 1607+ 导出，运行时解析而非静态
    // 依赖 SDK 版本宏；老系统回落 `GetDeviceCaps`（只取 Y 轴：Win32 两轴 DPI 同值，见下方注释）。
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI *)();
    // NOLINTBEGIN(*-pro-type-reinterpret-cast, *-casting-through-void)
    if (const auto f = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow")))) {
        dpi = static_cast<int>(f(hwnd));
    } else if (const auto f = reinterpret_cast<GetDpiForSystemFn>(
                   reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem")))) {
        dpi = static_cast<int>(f());
    } else {
        const HDC dc = GetDC(hwnd);
        if (dc != nullptr) {
            dpi = GetDeviceCaps(dc, LOGPIXELSY);  // Win32 两轴同值，故只取 Y 轴
            ReleaseDC(hwnd, dc);
        }
    }
    // NOLINTEND(*-pro-type-reinterpret-cast, *-casting-through-void)
    scale = dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
}

// dp ↔ 物理像素的**唯一**两个换算点。宿主内任何地方都不得再裸写 `* scale` / `/ scale`——
// 那样等于把换算复制到各处，某一处漏改就重现 §8.2 那种「帧 / 逻辑 / scale 三方记账发散」。
auto Win32Host::Impl::to_logical(int px, int py) const -> Point {
    return Point{.x = static_cast<float>(px) / scale, .y = static_cast<float>(py) / scale};
}

auto Win32Host::Impl::to_physical(Size dp) const -> Size {
    return Size{.width = dp.width * scale, .height = dp.height * scale};
}

// ---- 窗口过程：仅在最早时机（WM_NCCREATE/WM_CREATE）把 Impl* 存入 GWLP_USERDATA，
//      随后按消息族分发到对应 handle_* 处理函数（创建/输入/尺寸/绘制/关闭 等）。----
auto WINAPI Win32Host::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    // NOLINTBEGIN(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    if (msg == WM_NCCREATE || msg == WM_CREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTA *>(lp);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        if (msg == WM_NCCREATE) {
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
    }

    auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    // NOLINTEND(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    if (self == nullptr) {
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_CREATE:
            return handle_create();
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
            return self->handle_mouse(hwnd, msg, lp);
        case WM_MOUSEWHEEL:
            return self->handle_wheel(hwnd, wp, lp);
        case WM_KEYDOWN:
        case WM_KEYUP:
            return self->handle_key(msg, wp, lp);
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return self->handle_syskey(msg, wp, lp);
        case WM_CHAR:
            return self->handle_char(wp);
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
            return self->handle_ime(msg, wp, lp);
        case WM_KILLFOCUS:
            // 焦点交出即清空修饰态：抬起消息可能落到新获得焦点的窗口，本窗口的跟踪器再也收不到
            self->mods.clear();
            return self->handle_ime(msg, wp, lp);
        case WM_SIZE:
            return self->handle_size(hwnd, wp, lp);
        case WM_PAINT:
            return self->handle_paint(hwnd);
        case WM_ACTIVATE:
            return self->handle_activate(wp);
        case WM_DPICHANGED:
            return self->handle_dpi_changed(hwnd, wp, lp);
        case WM_GETMINMAXINFO:
            return self->handle_getminmaxinfo(lp);
        case WM_CLOSE:
            return self->handle_close();
        case WM_DESTROY:
            return self->handle_destroy();
        case WM_DROPFILES:
            return self->handle_dropfiles(hwnd, lp);
        case WM_GETOBJECT: {
            const std::optional<LRESULT> answered = self->handle_get_object(wp, lp);
            return answered.has_value() ? *answered : DefWindowProcA(hwnd, msg, wp, lp);
        }
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// ===== Win32Host 公共 API：全部委托给 pimpl_ =====
Win32Host::Win32Host(int w, int h, const std::string &title, const WindowStyleOptions &style,
                     WindowVisibility visibility)
    : pimpl_(std::make_unique<Impl>(w, h, title, style, visibility)) {}

Win32Host::~Win32Host() = default;

auto Win32Host::set_event_handler(EventHandler h) const -> void { pimpl_->handler = std::move(h); }
auto Win32Host::set_window_state_handler(WindowStateHandler h) const -> void {
    pimpl_->window_state_handler = std::move(h);
}
auto Win32Host::set_window_mode_handler(WindowModeHandler h) const -> void {
    pimpl_->window_mode_handler = std::move(h);
}
auto Win32Host::set_present_request(PresentRequest h) const -> void { pimpl_->present_request = std::move(h); }

auto Win32Host::set_title(const std::string &title) const -> void {
    if (pimpl_->hwnd != nullptr) {
        SetWindowTextA(pimpl_->hwnd, utf8_to_acp(title).c_str());
    }
}

[[nodiscard]] auto Win32Host::hwnd() const -> void * { return pimpl_->hwnd; }
[[nodiscard]] auto Win32Host::size() const -> Size { return pimpl_->size; }
[[nodiscard]] auto Win32Host::scale_factor() const -> float { return pimpl_->scale; }
[[nodiscard]] auto Win32Host::should_close() const -> bool { return pimpl_->should_close; }
[[nodiscard]] auto Win32Host::present_count() const -> int { return pimpl_->present_count; }
[[nodiscard]] auto Win32Host::background_brush() -> void * { return static_cast<void *>(Impl::bg_brush); }

auto Win32Host::poll_platform_events() const -> void {
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
        if (msg.message == WM_QUIT) {
            pimpl_->should_close = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

auto Win32Host::wait_events(double timeout_ms) const -> void {
    if (timeout_ms == 0.0 || pimpl_->should_close) {
        return;  // 无需等待 / 已请求关闭：立即回到循环退出判定
    }
    // 无限等待按 1000ms 分段兜底：即便个别唤醒渠道丢失（如窗口重建后句柄失效），
    // 主循环最迟 1s 自然醒一次重查状态，不死等；空转成本约 1 次/秒，可忽略。
    const double capped = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1000.0 : timeout_ms;
    const auto dw = static_cast<DWORD>(std::ceil(capped));
    // QS_ALLINPUT：任意队列消息（含 PostMessage 的 WM_NULL 唤醒）即返回；
    // MWMO_INPUTAVAILABLE：队列里已有未处理消息（上次 Peek 后新到/未抽完）时立即返回，
    // 避免「只等新输入」把已排队消息睡过去。
    MsgWaitForMultipleObjectsEx(0, nullptr, dw, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

auto Win32Host::request_wake() const -> void {
    if (pimpl_->hwnd != nullptr) {
        PostMessageA(pimpl_->hwnd, WM_NULL, 0, 0);  // 线程安全；空消息仅用于打断 wait_events
    }
}

// ---- 父子窗口与模态 ----

auto Win32Host::set_owner(void *owner_hwnd) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    // `GWLP_HWNDPARENT` 改变的是 **owner**（不是子窗口 parent）：子窗恒浮于 owner 之上、
    // 随 owner 最小化、不产生独立任务栏条目。传 nullptr 解除从属关系。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Win32 要求 HWND 以 LONG_PTR 传入 GWLP_*，属 API 约定
    SetWindowLongPtrA(pimpl_->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_hwnd));
}

auto Win32Host::set_enabled(bool on) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    EnableWindow(pimpl_->hwnd, on ? TRUE : FALSE);  // 模态窗口屏蔽其 owner 的输入
}

// ---- z 序、显示器与 DPI ----

auto Win32Host::raise() const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    BringWindowToTop(pimpl_->hwnd);  // 仅提 z 序，不改变激活状态
}

auto Win32Host::focus_window() const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    SetForegroundWindow(pimpl_->hwnd);
    SetFocus(pimpl_->hwnd);
}

auto Win32Host::display_id() const -> int {
    if (pimpl_->hwnd == nullptr) {
        return -1;
    }
    const HMONITOR hmon = MonitorFromWindow(pimpl_->hwnd, MONITOR_DEFAULTTONEAREST);
    if (hmon == nullptr) {
        return -1;
    }
    // 与 `app::Display::id` 同源：`display_win32.cpp` 以 HMONITOR 句柄值作稳定 id。
    // HMONITOR 为不透明句柄，取其句柄值是与 display_win32 的既定契约。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return static_cast<int>(reinterpret_cast<std::intptr_t>(hmon));
}

auto Win32Host::set_scale_change_handler(std::function<void(float)> h) const -> void {
    pimpl_->scale_handler = std::move(h);
}

// ---- 窗口几何（多窗口：几何持久化的读写端）----

auto Win32Host::position() const -> Point {
    if (pimpl_->hwnd == nullptr) {
        return Point{};
    }
    RECT r{};
    if (GetWindowRect(pimpl_->hwnd, &r) == 0) {
        return Point{};
    }
    return Point{.x = static_cast<float>(r.left), .y = static_cast<float>(r.top)};
}

auto Win32Host::set_position(Point p) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    SetWindowPos(pimpl_->hwnd, nullptr, static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

auto Win32Host::set_size(Size s) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    // 逻辑 dp → 物理客户区 → 物理外框：× scale 换算后做非客户区补偿（与构造路径同一
    // 口径），保证 set_size 后客户区尺寸 == 请求的逻辑尺寸。若把逻辑值直接当外框尺寸
    // 传 SetWindowPos，客户区会被 chrome 挤占（标题栏随 DPI 放大时尤甚），几何漂移。
    const auto win_style = static_cast<DWORD>(GetWindowLongPtrA(pimpl_->hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrA(pimpl_->hwnd, GWL_EXSTYLE));
    RECT rect{.left = 0,
              .top = 0,
              .right = static_cast<int>(std::lround(pimpl_->to_physical(s).width)),
              .bottom = static_cast<int>(std::lround(pimpl_->to_physical(s).height))};
    AdjustWindowRectEx(&rect, win_style, GetMenu(pimpl_->hwnd) != nullptr ? TRUE : FALSE, ex_style);
    SetWindowPos(pimpl_->hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace aurora

#endif  // AURORA_BACKEND_WIN32
