#include "aurora/window/win32_window.h"

#include "aurora/window/detail/win32_ua.h"
#include "aurora/window/detail/win32_ime.h"

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
#include <vector>

#include "aurora/core/utf8.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/window.h"
#include "aurora/window/window_state.h"

namespace aurora {

// ---- 自由函数：修饰键 / 键码映射 / UTF-8 转换（不依赖实例，纯函数）----
[[nodiscard]] static auto is_async_key_down(int vk) -> bool {
    // GetAsyncKeyState 返回有符号 SHORT；对最高位做位与时应先转无符号，
    // 避免 signed-bitwise 静态检查告警。
    return (static_cast<std::uint16_t>(GetAsyncKeyState(vk)) & 0x8000U) != 0U;
}

[[nodiscard]] static auto current_modifiers() -> ModifierKey {
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
    return m;
}

[[nodiscard]] static auto from_win32_vk(int vk) -> KeyCode {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::D0) + (vk - '0'));
    }
    switch (vk) {
        case VK_RETURN:
            return KeyCode::Enter;
        case VK_ESCAPE:
            return KeyCode::Escape;
        case VK_TAB:
            return KeyCode::Tab;
        case VK_BACK:
            return KeyCode::Backspace;
        case VK_DELETE:
            return KeyCode::Delete;
        case VK_SPACE:
            return KeyCode::Space;
        case VK_LEFT:
            return KeyCode::ArrowLeft;
        case VK_RIGHT:
            return KeyCode::ArrowRight;
        case VK_UP:
            return KeyCode::ArrowUp;
        case VK_DOWN:
            return KeyCode::ArrowDown;
        case VK_SHIFT:
            return KeyCode::Shift;
        case VK_CONTROL:
            return KeyCode::Control;
        case VK_MENU:
            return KeyCode::Alt;
        case VK_LWIN:
        case VK_RWIN:
            return KeyCode::Meta;
        case VK_HOME:
            return KeyCode::Home;
        case VK_END:
            return KeyCode::End;
        case VK_PRIOR:
            return KeyCode::PageUp;
        case VK_NEXT:
            return KeyCode::PageDown;
        case VK_OEM_MINUS:
            return KeyCode::Minus;
        case VK_OEM_PLUS:
            return KeyCode::Equal;
        case VK_OEM_1:
            return KeyCode::Semicolon;
        case VK_OEM_7:
            return KeyCode::Quote;
        case VK_OEM_COMMA:
            return KeyCode::Comma;
        case VK_OEM_PERIOD:
            return KeyCode::Period;
        case VK_OEM_2:
            return KeyCode::Slash;
        case VK_OEM_3:
            return KeyCode::Backquote;
        case VK_OEM_4:
            return KeyCode::LeftBracket;
        case VK_OEM_6:
            return KeyCode::RightBracket;
        case VK_OEM_5:
            return KeyCode::Backslash;
        default:
            if (vk >= VK_F1 && vk <= VK_F12) {
                return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (vk - VK_F1));
            }
            return KeyCode::Unknown;
    }
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
struct Win32Window::Impl {
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    Size size{.width = 0.0F, .height = 0.0F};
    float scale = 1.0F;  ///< device pixel ratio（dp → 物理像素）
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
    /// @brief 无障碍桥（G14：由窗口宿主持有，GDI / D3D11 两个 Surface 共用同一实例）。
    std::unique_ptr<detail::Win32UiaBridge> a11y;
    /// @brief 宿主接管 `WM_GETOBJECT` 的钩子（默认空 → 走内置桥）。
    std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> a11y_hook;
    /// @brief 最近一次注入的语义树根（非拥有；桥尚未构造时先由宿主记下）。
    Widget *a11y_root = nullptr;

    /// @brief IMM32 组合输入桥（与窗口同生命周期；无输入法激活时零消息、零成本）。
    std::unique_ptr<detail::Win32ImeBridge> ime;
    /// @brief 焦点控件的候选窗定位盒查询（由 `WindowHost` 注入；空 = 无定位，走系统默认）。
    std::function<Rect()> composition_caret_provider;

    inline static bool class_registered = false;
    inline static HBRUSH bg_brush = nullptr;  ///< 浅色背景擦除刷（消除最大化黑屏），注册时创建一次。
    static constexpr auto AURORA_CLASS_NAME = "AuroraWin32Surface";

    Impl(int w, int h, const std::string &title, const WindowStyleOptions &style);
    ~Impl();
    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    // 事件翻译（输入分族内部调用）
    auto on_mouse(MouseAction action, MouseButton button, int x, int y) const -> void;
    auto on_wheel(int delta, int x, int y) const -> void;
    auto on_key(KeyAction action, int vk) const -> void;
    auto on_char(std::uint32_t ch) const -> void;

    /// @brief 由最小化/激活标志重算可见性状态，仅实际改变时上报（避免重复通知）。
    auto update_window_state() -> void;

    // ---- 消息分族处理函数（wnd_proc 按消息族分发到此处）----
    static auto handle_create() -> LRESULT;
    auto handle_mouse(HWND hwnd_in, UINT msg, LPARAM lp) -> LRESULT;
    auto handle_wheel(HWND hwnd_in, WPARAM wp, LPARAM lp) const -> LRESULT;
    [[nodiscard]] auto handle_key(UINT msg, WPARAM wp) const -> LRESULT;
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
    [[nodiscard]] auto handle_ime(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
    auto handle_dropfiles(HWND hwnd_in, LPARAM lp) const -> LRESULT;

    auto register_class() const -> void;
    [[nodiscard]] auto dpi_scale() const -> float;

    // 窗口过程：仅在最早时机（WM_NCCREATE/WM_CREATE）把 Impl* 存入 GWLP_USERDATA，
    // 随后按消息族分发到对应 handle_* 处理函数（创建/输入/尺寸/绘制/关闭 等）。
    static auto WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT;
};

// ---- Impl 构造：窗口创建 + DPI 适配 + 类注册 + 显示 ----
Win32Window::Impl::Impl(int w, int h, const std::string &title, const WindowStyleOptions &style)
    : scale(dpi_scale()),  // 创建时主显示器 DPI
      style(style) {
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

    // DPI 感知下窗口坐标即物理像素：物理窗口尺寸 = 逻辑 dp × scale。
    RECT rect{.left = 0,
              .top = 0,
              .right = static_cast<int>(std::lround(static_cast<float>(w) * scale)),
              .bottom = static_cast<int>(std::lround(static_cast<float>(h) * scale))};
    AdjustWindowRect(&rect, win_style, FALSE);
    const int win_w = rect.right - rect.left;
    const int win_h = rect.bottom - rect.top;

    // 标题为 UTF-8；CreateWindowExA 按进程 ANSI 代码页解释字节，故先转 UTF-8 → ACP 再传 ANSI API。
    const std::string ansi_title = utf8_to_acp(title);
    // 把 Impl* 作为 lpParam 传入，WM_NCCREATE 时存入 GWLP_USERDATA（wnd_proc 取回）。
    hwnd = CreateWindowExA(ex_style, AURORA_CLASS_NAME, ansi_title.c_str(), win_style, CW_USEDEFAULT, CW_USEDEFAULT,
                           win_w, win_h, nullptr, nullptr, hinst, this);

    if (hwnd != nullptr) {
        if (style.always_on_top) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        DragAcceptFiles(hwnd, TRUE);  // 启用操作系统文件拖放（WM_DROPFILES）
        // IMM32 组合桥：桥自身只吃 WM_IME_*，无输入法时一条也不来，故随窗口直接构造。
        ime = std::make_unique<detail::Win32ImeBridge>(
            hwnd, detail::Win32ImeBridge::Hooks{
                      .emit = [this](Event &e) -> void {
                          if (handler) {
                              handler(e);
                          }
                      },
                      .caret_bounds = [this]() -> Rect {
                          return composition_caret_provider ? composition_caret_provider() : Rect{};
                      },
                      .scale_factor = [this]() -> float { return scale; }});
    }
    size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};  // 逻辑 dp（布局用）
}

Win32Window::Impl::~Impl() {
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
    ime.reset();  // 桥的 hooks 捕获本 Impl，须在 DestroyWindow 前先散
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

auto Win32Window::Impl::on_mouse(MouseAction action, MouseButton button, int x, int y) const -> void {
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
    e.position = Point{.x = static_cast<float>(x) / scale, .y = static_cast<float>(y) / scale};
    handler(e);
}

auto Win32Window::Impl::on_wheel(int delta, int x, int y) const -> void {
    if (!handler) {
        return;
    }
    ScrollEvent e;
    e.position = Point{.x = static_cast<float>(x) / scale, .y = static_cast<float>(y) / scale};
    e.delta_y = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
    handler(e);
}

auto Win32Window::Impl::on_key(KeyAction action, int vk) const -> void {
    if (!handler) {
        return;
    }
    KeyEvent e;
    e.action = action;
    e.key = static_cast<int>(from_win32_vk(vk));
    e.modifiers = current_modifiers();
    handler(e);
}

auto Win32Window::Impl::on_char(std::uint32_t ch) const -> void {
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

auto Win32Window::Impl::update_window_state() -> void {
    const WindowState want = compute_window_state(minimized, active);
    if (want != state) {
        state = want;
        if (window_state_handler) {
            window_state_handler(want);
        }
    }
}

// ---- 创建分族 ----
auto Win32Window::Impl::handle_create() -> LRESULT { return 0; }

// ---- 输入分族（鼠标按钮 / 移动 / 离开 / 滚轮 / 键 / 字符）----
auto Win32Window::Impl::handle_mouse(HWND hwnd_in, UINT msg, LPARAM lp) -> LRESULT {
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

auto Win32Window::Impl::handle_wheel(HWND hwnd_in, WPARAM wp, LPARAM lp) const -> LRESULT {
    POINT pt{.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_in, &pt);
    on_wheel(GET_WHEEL_DELTA_WPARAM(wp), pt.x, pt.y);
    return 0;
}

auto Win32Window::Impl::handle_key(UINT msg, WPARAM wp) const -> LRESULT {
    on_key((msg == WM_KEYUP) ? KeyAction::Up : KeyAction::Down, static_cast<int>(wp));
    return 0;
}

auto Win32Window::Impl::handle_char(WPARAM wp) const -> LRESULT {
    // 组合期残余 WM_CHAR 整条丢弃：IME 上屏结果统一走 `GCS_RESULTSTR`（见 win32_ime 桥），
    // 两条通道并存即「同一汉字上屏两次」。
    if (ime != nullptr && ime->is_composing()) {
        return 0;
    }
    on_char(static_cast<std::uint32_t>(wp));
    return 0;
}

// ---- 尺寸分族（WM_SIZE）----
auto Win32Window::Impl::handle_size(HWND hwnd_in, WPARAM wp, LPARAM lp) -> LRESULT {
    // DPI 感知下 lParam 为物理像素；换算回逻辑 dp 供布局使用。
    const int pw = static_cast<int>(LOWORD(lp));
    const int ph = static_cast<int>(HIWORD(lp));
    if (pw > 0 && ph > 0) {
        const float sc = dpi_scale();
        size = Size{.width = static_cast<float>(pw) / sc, .height = static_cast<float>(ph) / sc};
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
auto Win32Window::Impl::handle_paint(HWND hwnd_in) -> LRESULT {
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
auto Win32Window::Impl::handle_activate(WPARAM wp) -> LRESULT {
    const bool new_active = (LOWORD(wp) != WA_INACTIVE);
    if (new_active != active) {
        active = new_active;
        update_window_state();
    }
    return 0;
}

// ---- DPI 变化分族（WM_DPICHANGED）----
auto Win32Window::Impl::handle_dpi_changed(HWND hwnd, WPARAM wp, LPARAM lp) -> LRESULT {
    const auto *pr = reinterpret_cast<RECT *>(lp);  // NOLINT(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    // wParam 的 HIWORD 为系统建议的新 Y 轴 DPI：更新逻辑缩放并上报表层，
    // 由 `Window` 强制全量重排重绘（否则 logical↔physical 换算失配会内容错位/发虚）。
    const int dpi_y = static_cast<int>(HIWORD(wp));
    if (dpi_y > 0) {
        scale = static_cast<float>(dpi_y) / 96.0F;
        notify_scale_changed();
    }
    return 0;
}

auto Win32Window::Impl::notify_scale_changed() const -> void {
    if (scale_handler) {
        scale_handler(scale);
    }
}

// ---- 尺寸限制分族（WM_GETMINMAXINFO）----
auto Win32Window::Impl::handle_getminmaxinfo(LPARAM lp) const -> LRESULT {
    // 尺寸限制：逻辑 dp × scale → 物理像素（含非客户区补偿）。
    auto *mmi = reinterpret_cast<MINMAXINFO *>(lp);  // NOLINT(*-pro-type-reinterpret-cast, *-no-int-to-ptr)
    const float sc = dpi_scale();
    const WindowStyleOptions &st = style;
    if (st.min_size.width > 0.0F) {
        mmi->ptMinTrackSize.x = static_cast<LONG>(std::lround(st.min_size.width * sc));
    }
    if (st.min_size.height > 0.0F) {
        mmi->ptMinTrackSize.y = static_cast<LONG>(std::lround(st.min_size.height * sc));
    }
    if (st.max_size.width > 0.0F) {
        mmi->ptMaxTrackSize.x = static_cast<LONG>(std::lround(st.max_size.width * sc));
    }
    if (st.max_size.height > 0.0F) {
        mmi->ptMaxTrackSize.y = static_cast<LONG>(std::lround(st.max_size.height * sc));
    }
    return 0;
}

// ---- 关闭分族（WM_CLOSE / WM_DESTROY）----
auto Win32Window::Impl::handle_close() -> LRESULT {
    should_close = true;
    return 0;
}

auto Win32Window::Impl::handle_destroy() -> LRESULT {
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

// ---- 无障碍分族（WM_GETOBJECT → UIA 桥，D14）----
auto Win32Window::Impl::handle_get_object(WPARAM wp, LPARAM lp) -> std::optional<LRESULT> {
    if (a11y_hook) {
        // 公共签名用指针宽度整数（避免公共头引入 <windows.h>），此处还原为原生类型。
        if (auto answered = a11y_hook(static_cast<std::uintptr_t>(wp), static_cast<std::intptr_t>(lp));
            answered.has_value()) {
            return static_cast<LRESULT>(*answered);  // 宿主接管
        }
    }
    constexpr LONG UIA_ROOT_OBJECT_ID = -25;
    if (static_cast<LONG>(static_cast<DWORD>(lp)) != UIA_ROOT_OBJECT_ID) {
        return std::nullopt;  // 非 UIA 根请求：交 DefWindowProc（MSAA 兜底）
    }
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    if (a11y == nullptr) {
        // 惰性构造（D14）：无读屏在线时连桥对象都不存在 ⇒ 零开销。
        a11y = std::make_unique<detail::Win32UiaBridge>(hwnd);
        a11y->set_root(a11y_root);  // 补喂：宿主在桥存在前已记下的根
    }
    return a11y->handle_get_object(wp, lp);
}

// ---- 输入法分族（WM_IME_* → IMM32 组合桥）----
auto Win32Window::Impl::handle_ime(UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
    if (ime != nullptr) {
        if (const std::optional<LRESULT> answered = ime->handle(msg, lp); answered.has_value()) {
            return *answered;
        }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

auto Win32Window::accessibility_provider() const -> a11y::Provider * { return pimpl_->a11y.get(); }

auto Win32Window::set_accessibility_hook(std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> h)
    const -> void {
    pimpl_->a11y_hook = std::move(h);
}

auto Win32Window::set_accessibility_root(Widget *root) const -> void {
    pimpl_->a11y_root = root;
    if (pimpl_->a11y != nullptr) {
        pimpl_->a11y->set_root(root);
    }
}

auto Win32Window::set_composition_caret_provider(std::function<Rect()> provider) const -> void {
    pimpl_->composition_caret_provider = std::move(provider);
}

auto Win32Window::ime_composing() const -> bool { return pimpl_->ime != nullptr && pimpl_->ime->is_composing(); }

// ---- 文件拖放分族（WM_DROPFILES）----
auto Win32Window::Impl::handle_dropfiles(HWND hwnd_in, LPARAM lp) const -> LRESULT {
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
    fde.position = Point{.x = static_cast<float>(pt.x) / scale, .y = static_cast<float>(pt.y) / scale};
    fde.paths = std::move(paths);
    if (handler) {
        handler(fde);
    }
    return 0;
}

auto Win32Window::Impl::register_class() const -> void {
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

auto Win32Window::Impl::dpi_scale() const -> float {
    int dpi = 96;
    const HDC dc = (hwnd != nullptr) ? GetDC(hwnd) : GetDC(nullptr);
    if (dc != nullptr) {
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
    }
    return dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
}

// ---- 窗口过程：仅在最早时机（WM_NCCREATE/WM_CREATE）把 Impl* 存入 GWLP_USERDATA，
//      随后按消息族分发到对应 handle_* 处理函数（创建/输入/尺寸/绘制/关闭 等）。----
auto WINAPI Win32Window::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
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
            return self->handle_key(msg, wp);
        case WM_CHAR:
            return self->handle_char(wp);
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
        case WM_KILLFOCUS:
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

// ===== Win32Window 公共 API：全部委托给 pimpl_ =====
Win32Window::Win32Window(int w, int h, const std::string &title, const WindowStyleOptions &style)
    : pimpl_(std::make_unique<Impl>(w, h, title, style)) {}

Win32Window::~Win32Window() = default;

auto Win32Window::set_event_handler(EventHandler h) const -> void { pimpl_->handler = std::move(h); }
auto Win32Window::set_window_state_handler(WindowStateHandler h) const -> void {
    pimpl_->window_state_handler = std::move(h);
}
auto Win32Window::set_window_mode_handler(WindowModeHandler h) const -> void {
    pimpl_->window_mode_handler = std::move(h);
}
auto Win32Window::set_present_request(PresentRequest h) const -> void { pimpl_->present_request = std::move(h); }

auto Win32Window::set_title(const std::string &title) const -> void {
    if (pimpl_->hwnd != nullptr) {
        SetWindowTextA(pimpl_->hwnd, utf8_to_acp(title).c_str());
    }
}

[[nodiscard]] auto Win32Window::hwnd() const -> void * { return pimpl_->hwnd; }
[[nodiscard]] auto Win32Window::size() const -> Size { return pimpl_->size; }
[[nodiscard]] auto Win32Window::scale_factor() const -> float { return pimpl_->scale; }
[[nodiscard]] auto Win32Window::should_close() const -> bool { return pimpl_->should_close; }
[[nodiscard]] auto Win32Window::present_count() const -> int { return pimpl_->present_count; }
[[nodiscard]] auto Win32Window::background_brush() -> void * { return static_cast<void *>(Impl::bg_brush); }

auto Win32Window::poll_platform_events() const -> void {
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

auto Win32Window::wait_events(double timeout_ms) const -> void {
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

auto Win32Window::request_wake() const -> void {
    if (pimpl_->hwnd != nullptr) {
        PostMessageA(pimpl_->hwnd, WM_NULL, 0, 0);  // 线程安全；空消息仅用于打断 wait_events
    }
}

// ---- 父子窗口与模态 ----

auto Win32Window::set_owner(void *owner_hwnd) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    // `GWLP_HWNDPARENT` 改变的是 **owner**（不是子窗口 parent）：子窗恒浮于 owner 之上、
    // 随 owner 最小化、不产生独立任务栏条目。传 nullptr 解除从属关系。
    SetWindowLongPtrA(pimpl_->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_hwnd));
}

auto Win32Window::set_enabled(bool on) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    EnableWindow(pimpl_->hwnd, on ? TRUE : FALSE);  // 模态窗口屏蔽其 owner 的输入
}

// ---- z 序、显示器与 DPI ----

auto Win32Window::raise() const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    BringWindowToTop(pimpl_->hwnd);  // 仅提 z 序，不改变激活状态
}

auto Win32Window::focus_window() const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    SetForegroundWindow(pimpl_->hwnd);
    SetFocus(pimpl_->hwnd);
}

auto Win32Window::display_id() const -> int {
    if (pimpl_->hwnd == nullptr) {
        return -1;
    }
    const HMONITOR hmon = MonitorFromWindow(pimpl_->hwnd, MONITOR_DEFAULTTONEAREST);
    if (hmon == nullptr) {
        return -1;
    }
    // 与 `app::Display::id` 同源：`display_win32.cpp` 以 HMONITOR 句柄值作稳定 id。
    return static_cast<int>(reinterpret_cast<std::intptr_t>(hmon));
}

auto Win32Window::set_scale_change_handler(std::function<void(float)> h) const -> void {
    pimpl_->scale_handler = std::move(h);
}

// ---- 窗口几何（多窗口：几何持久化的读写端）----

auto Win32Window::position() const -> Point {
    if (pimpl_->hwnd == nullptr) {
        return Point{};
    }
    RECT r{};
    if (GetWindowRect(pimpl_->hwnd, &r) == 0) {
        return Point{};
    }
    return Point{.x = static_cast<float>(r.left), .y = static_cast<float>(r.top)};
}

auto Win32Window::set_position(Point p) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    SetWindowPos(pimpl_->hwnd, nullptr, static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

auto Win32Window::set_size(Size s) const -> void {
    if (pimpl_->hwnd == nullptr) {
        return;
    }
    SetWindowPos(pimpl_->hwnd, nullptr, 0, 0, static_cast<int>(std::lround(s.width)),
                 static_cast<int>(std::lround(s.height)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace aurora

#endif  // AURORA_BACKEND_WIN32
