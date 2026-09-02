#include "aurora/window/glfw_surface.h"

#include "aurora/core/platform.h"

#ifdef AURORA_BACKEND_GLFW

#include <GL/gl.h>
#include <GLFW/glfw3.h>

#ifdef AURORA_PLATFORM_WINDOWS
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "aurora/core/utf8.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/win32_capture.h"
#include "aurora/window/window_state.h"

namespace aurora {

// ---- 键码 / 修饰键 / UTF-8 翻译（纯函数，static 成员）----
[[nodiscard]] static auto from_glfw_key(int key) -> KeyCode {
    switch (key) {
    case GLFW_KEY_A: return KeyCode::A;
    case GLFW_KEY_B: return KeyCode::B;
    case GLFW_KEY_C: return KeyCode::C;
    case GLFW_KEY_D: return KeyCode::D;
    case GLFW_KEY_E: return KeyCode::E;
    case GLFW_KEY_F: return KeyCode::F;
    case GLFW_KEY_G: return KeyCode::G;
    case GLFW_KEY_H: return KeyCode::H;
    case GLFW_KEY_I: return KeyCode::I;
    case GLFW_KEY_J: return KeyCode::J;
    case GLFW_KEY_K: return KeyCode::K;
    case GLFW_KEY_L: return KeyCode::L;
    case GLFW_KEY_M: return KeyCode::M;
    case GLFW_KEY_N: return KeyCode::N;
    case GLFW_KEY_O: return KeyCode::O;
    case GLFW_KEY_P: return KeyCode::P;
    case GLFW_KEY_Q: return KeyCode::Q;
    case GLFW_KEY_R: return KeyCode::R;
    case GLFW_KEY_S: return KeyCode::S;
    case GLFW_KEY_T: return KeyCode::T;
    case GLFW_KEY_U: return KeyCode::U;
    case GLFW_KEY_V: return KeyCode::V;
    case GLFW_KEY_W: return KeyCode::W;
    case GLFW_KEY_X: return KeyCode::X;
    case GLFW_KEY_Y: return KeyCode::Y;
    case GLFW_KEY_Z: return KeyCode::Z;
    case GLFW_KEY_0: return KeyCode::D0;
    case GLFW_KEY_1: return KeyCode::D1;
    case GLFW_KEY_2: return KeyCode::D2;
    case GLFW_KEY_3: return KeyCode::D3;
    case GLFW_KEY_4: return KeyCode::D4;
    case GLFW_KEY_5: return KeyCode::D5;
    case GLFW_KEY_6: return KeyCode::D6;
    case GLFW_KEY_7: return KeyCode::D7;
    case GLFW_KEY_8: return KeyCode::D8;
    case GLFW_KEY_9: return KeyCode::D9;
    case GLFW_KEY_ESCAPE: return KeyCode::Escape;
    case GLFW_KEY_ENTER: return KeyCode::Enter;
    case GLFW_KEY_TAB: return KeyCode::Tab;
    case GLFW_KEY_BACKSPACE: return KeyCode::Backspace;
    case GLFW_KEY_DELETE: return KeyCode::Delete;
    case GLFW_KEY_SPACE: return KeyCode::Space;
    case GLFW_KEY_LEFT: return KeyCode::ArrowLeft;
    case GLFW_KEY_RIGHT: return KeyCode::ArrowRight;
    case GLFW_KEY_UP: return KeyCode::ArrowUp;
    case GLFW_KEY_DOWN: return KeyCode::ArrowDown;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT: return KeyCode::Shift;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return KeyCode::Control;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT: return KeyCode::Alt;
    case GLFW_KEY_LEFT_SUPER:
    case GLFW_KEY_RIGHT_SUPER: return KeyCode::Meta;
    case GLFW_KEY_HOME: return KeyCode::Home;
    case GLFW_KEY_END: return KeyCode::End;
    case GLFW_KEY_PAGE_UP: return KeyCode::PageUp;
    case GLFW_KEY_PAGE_DOWN: return KeyCode::PageDown;
    case GLFW_KEY_MINUS: return KeyCode::Minus;
    case GLFW_KEY_EQUAL: return KeyCode::Equal;
    case GLFW_KEY_LEFT_BRACKET: return KeyCode::LeftBracket;
    case GLFW_KEY_RIGHT_BRACKET: return KeyCode::RightBracket;
    case GLFW_KEY_BACKSLASH: return KeyCode::Backslash;
    case GLFW_KEY_SEMICOLON: return KeyCode::Semicolon;
    case GLFW_KEY_APOSTROPHE: return KeyCode::Quote;
    case GLFW_KEY_COMMA: return KeyCode::Comma;
    case GLFW_KEY_PERIOD: return KeyCode::Period;
    case GLFW_KEY_SLASH: return KeyCode::Slash;
    case GLFW_KEY_GRAVE_ACCENT: return KeyCode::Backquote;
    case GLFW_KEY_F1: return KeyCode::F1;
    case GLFW_KEY_F2: return KeyCode::F2;
    case GLFW_KEY_F3: return KeyCode::F3;
    case GLFW_KEY_F4: return KeyCode::F4;
    case GLFW_KEY_F5: return KeyCode::F5;
    case GLFW_KEY_F6: return KeyCode::F6;
    case GLFW_KEY_F7: return KeyCode::F7;
    case GLFW_KEY_F8: return KeyCode::F8;
    case GLFW_KEY_F9: return KeyCode::F9;
    case GLFW_KEY_F10: return KeyCode::F10;
    case GLFW_KEY_F11: return KeyCode::F11;
    case GLFW_KEY_F12: return KeyCode::F12;
    default: return KeyCode::Unknown;
    }
}

[[nodiscard]] static auto glfw_mods_to_aurora(int mods) -> ModifierKey {
    auto m = ModifierKey::None;
    if ((mods & GLFW_MOD_SHIFT) != 0) { // NOLINT(*-signed-bitwise)
        m = m | ModifierKey::Shift;
    }
    if ((mods & GLFW_MOD_CONTROL) != 0) { // NOLINT(*-signed-bitwise)
        m = m | ModifierKey::Control;
    }
    if ((mods & GLFW_MOD_ALT) != 0) { // NOLINT(*-signed-bitwise)
        m = m | ModifierKey::Alt;
    }
    if ((mods & GLFW_MOD_SUPER) != 0) { // NOLINT(*-signed-bitwise)
        m = m | ModifierKey::Meta;
    }
    return m;
}

// pimpl：全部 GLFW/OpenGL 状态与回调都在这里；公共头仅持有 unique_ptr<Impl>。
struct GlfwSurface::Impl {
    GLFWwindow *m_window = nullptr;
    Size m_logical_size{ .width = 800.0f, .height = 600.0f };
    float m_scale = 1.0f;
    Painter m_painter;
    int m_painter_w = 0;
    int m_painter_h = 0;
    int m_frame = 0;
    EventHandler m_handler;
    bool m_minimized = false;                   ///< 是否最小化（GLFW iconify 回调）。
    bool m_active = true;                       ///< 是否前台激活（GLFW focus 回调）。初值 true：创建即激活。
    bool m_maximized = false;                   ///< 是否最大化（GLFW maximize 回调）。
    WindowMode m_mode = WindowMode::Normal;     ///< 当前几何态（变化时上报）。
    WindowState m_state = WindowState::Visible; ///< 当前可见性状态（变化时上报）。

    // ---- GL 资源（OpenGL 1.1 立即模式，仅需纹理对象）----
    bool m_gl_ready = false;
    GLuint m_tex = 0;
    int m_tex_w = 0;
    int m_tex_h = 0;

    WindowStateHandler m_window_state_handler;
    WindowModeHandler m_window_mode_handler;

    explicit Impl(const Config &cfg);
    ~Impl();

    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    auto begin_frame(int width, int height) -> Result<bool>;
    auto painter() -> Painter & { return m_painter; }
    auto present() -> Result<bool>;
    [[nodiscard]] auto size() const -> Size { return m_logical_size; }
    [[nodiscard]] auto scale_factor() const -> float { return m_scale; }
    [[nodiscard]] auto should_close() const -> bool { return glfwWindowShouldClose(m_window) == GLFW_TRUE; }
    static auto poll_platform_events() -> void { glfwPollEvents(); }
    auto wait_events(double timeout_ms) const -> void;
    static auto request_wake() -> void { glfwPostEmptyEvent(); }
    [[nodiscard]] auto data() const -> const std::uint8_t * { return m_painter.data(); }
    [[nodiscard]] auto frame_count() const -> int { return m_frame; }

    auto ensure_gl_objects() -> void;
    auto upload_and_draw() -> void;

    // ---- GLFW 回调（C 链接，static 转发到 Impl）----
    static auto on_cursor_pos(GLFWwindow *w, double x, double y) -> void;
    static auto on_mouse_button(GLFWwindow *w, int button, int action, int mods) -> void;
    static auto on_key(GLFWwindow *w, int key, int scancode, int action, int mods) -> void;
    static auto on_scroll(GLFWwindow *w, double xoff, double yoff) -> void;
    static auto on_char(GLFWwindow *w, unsigned int codepoint) -> void;
    static auto on_window_size(GLFWwindow *w, int width, int height) -> void;
    static auto on_window_iconify(GLFWwindow *w, int iconified) -> void;
    static auto on_window_focus(GLFWwindow *w, int focused) -> void;
    static auto on_window_maximize(GLFWwindow *w, int maximized) -> void;

    /// @brief 坐标换算：GLFW 光标位置已是内容坐标，与 aurora 逻辑坐标空间一致，故恒等。
    [[nodiscard]] static auto to_logical(double x, double y) -> Point {
        return Point{ .x = static_cast<float>(x), .y = static_cast<float>(y) };
    }

    /// @brief 由最小化/最大化标志重算几何态，仅实际改变时上报。
    auto update_window_mode() -> void {
        const WindowMode want = compute_window_mode(m_minimized, m_maximized, false);
        if (want != m_mode) {
            m_mode = want;
            if (m_window_mode_handler) {
                m_window_mode_handler(want);
            }
        }
    }

    /// @brief 由最小化/激活标志重算可见性状态，仅实际改变时上报。
    auto update_window_state() -> void {
        const WindowState want = compute_window_state(m_minimized, m_active);
        if (want != m_state) {
            m_state = want;
            if (m_window_state_handler) {
                m_window_state_handler(want);
            }
        }
    }
};

GlfwSurface::Impl::Impl(const Config &cfg) {
    if (glfwInit() == 0) {
        throw std::runtime_error("GlfwSurface: glfwInit failed");
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, cfg.gl_major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, cfg.gl_minor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, cfg.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(static_cast<int>(cfg.size.width), static_cast<int>(cfg.size.height), cfg.title.c_str(),
                                nullptr, nullptr);
    if (m_window == nullptr) {
        glfwTerminate();
        throw std::runtime_error("GlfwSurface: glfwCreateWindow failed");
    }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // 启用 VSync（帧循环调度，见 specification/06-app-platform.md §3.1）

    // 转发 GLFW 回调到本实例（ARCHITECTURE.md §3.1 事件来源）。用户指针存 Impl*，回调据此取回。
    glfwSetWindowUserPointer(m_window, this);
    glfwSetCursorPosCallback(m_window, &Impl::on_cursor_pos);
    glfwSetMouseButtonCallback(m_window, &Impl::on_mouse_button);
    glfwSetKeyCallback(m_window, &Impl::on_key);
    glfwSetScrollCallback(m_window, &Impl::on_scroll);
    glfwSetCharCallback(m_window, &Impl::on_char);
    glfwSetWindowSizeCallback(m_window, &Impl::on_window_size);
    glfwSetWindowIconifyCallback(m_window, &Impl::on_window_iconify);
    glfwSetWindowFocusCallback(m_window, &Impl::on_window_focus);
    glfwSetWindowMaximizeCallback(m_window, &Impl::on_window_maximize);

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(m_window, &xscale, &yscale);
    m_scale = xscale; // 假设各向同性 DPI
}

GlfwSurface::Impl::~Impl() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
    }
    glfwTerminate();
}

auto GlfwSurface::Impl::begin_frame(int /*width*/, int /*height*/) -> Result<bool> {
    int c_w = 0;
    int c_h = 0;
    glfwGetWindowSize(m_window, &c_w, &c_h);
    if (c_w <= 0) {
        c_w = static_cast<int>(m_logical_size.width);
    }
    if (c_h <= 0) {
        c_h = static_cast<int>(m_logical_size.height);
    }

    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(m_window, &fb_w, &fb_h);
    if (fb_w <= 0) {
        fb_w = c_w;
    }
    if (fb_h <= 0) {
        fb_h = c_h;
    }

    // aurora 逻辑坐标空间 == GLFW 内容坐标空间，故逻辑尺寸即内容尺寸。
    m_logical_size = Size{ .width = static_cast<float>(c_w), .height = static_cast<float>(c_h) };
    if (c_w != m_painter_w || c_h != m_painter_h) {
        m_painter.begin(c_w, c_h); // 软件栅格缓冲按逻辑尺寸；呈现时拉伸到 framebuffer。
        m_painter_w = c_w;
        m_painter_h = c_h;
    }

    // 每帧用浅色背景清空软件帧缓冲：默认文字为黑色，需要浅色底才能可见
    // （widget 默认 Color::black()；此前全屏纹理为透明黑导致黑底黑字不可见）。
    m_painter.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                              .size = Size{ .width = static_cast<float>(c_w), .height = static_cast<float>(c_h) } },
                        Color{ 245, 245, 247, 255 });

    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.961f, 0.961f, 0.969f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return Result<bool>{ true };
}

auto GlfwSurface::Impl::present() -> Result<bool> {
    upload_and_draw();
    glfwSwapBuffers(m_window);
    ++m_frame;
    return Result<bool>{ true };
}

auto GlfwSurface::Impl::ensure_gl_objects() -> void {
    if (m_gl_ready) {
        return;
    }
    // 仅创建纹理对象；顶点用立即模式绘制（见 uploadAndDraw）。
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_gl_ready = true;
}

auto GlfwSurface::Impl::upload_and_draw() -> void {
    ensure_gl_objects();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    if (m_tex_w != m_painter_w || m_tex_h != m_painter_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_painter_w, m_painter_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     m_painter.data());
        m_tex_w = m_painter_w;
        m_tex_h = m_painter_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_painter_w, m_painter_h, GL_RGBA, GL_UNSIGNED_BYTE, m_painter.data());
    }
    // 立即模式全屏四边形：UV 翻转使软件帧缓冲（上→下）正确呈现为直立图像。
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(1.0f, 1.0f);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

auto GlfwSurface::Impl::wait_events(double timeout_ms) const -> void {
    if (timeout_ms == 0.0 || should_close()) {
        return;
    }
    const double capped_s = (timeout_ms < 0.0 || timeout_ms > 1000.0) ? 1.0 : timeout_ms / 1000.0;
    glfwWaitEventsTimeout(capped_s);
}

// ---- GLFW 回调：从用户指针取回 Impl* 并翻译为 aurora Event ----
auto GlfwSurface::Impl::on_cursor_pos(GLFWwindow *w, double x, double y) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->m_handler) {
        return;
    }
    MouseEvent e;
    e.action = MouseAction::Move;
    e.button = MouseButton::Left;
    e.position = to_logical(x, y);
    self->m_handler(e);
}

auto GlfwSurface::Impl::on_mouse_button(GLFWwindow *w, int button, int action, int /*mods*/) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->m_handler) {
        return;
    }
    MouseEvent e;
    e.action = (action == GLFW_PRESS) ? MouseAction::Press : MouseAction::Release;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        e.button = MouseButton::Right;
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        e.button = MouseButton::Middle;
    } else {
        e.button = MouseButton::Left;
    }
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    e.position = to_logical(x, y);
    self->m_handler(e);
}

auto GlfwSurface::Impl::on_key(GLFWwindow *w, int key, int /*scancode*/, int action, int mods) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->m_handler) {
        return;
    }
    KeyEvent e;
    e.key = static_cast<int>(from_glfw_key(key));
    e.action = (action == GLFW_RELEASE) ? KeyAction::Up : KeyAction::Down;
    e.modifiers = glfw_mods_to_aurora(mods);
    self->m_handler(e);
}

auto GlfwSurface::Impl::on_scroll(GLFWwindow *w, double xoff, double yoff) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->m_handler) {
        return;
    }
    ScrollEvent e;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    e.position = to_logical(x, y);
    e.delta_x = static_cast<float>(xoff);
    e.delta_y = static_cast<float>(yoff);
    self->m_handler(e);
}

auto GlfwSurface::Impl::on_char(GLFWwindow *w, unsigned int codepoint) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->m_handler) {
        return;
    }
    TextInputEvent e;
    e.text = utf8_encode(codepoint);
    self->m_handler(e);
}

auto GlfwSurface::Impl::on_window_size(GLFWwindow *w, int /*width*/, int /*height*/) -> void {
    // 实际尺寸在 beginFrame 中读取并应用；此处仅确保事件被消费。
    (void)w;
}

auto GlfwSurface::Impl::on_window_iconify(GLFWwindow *w, int iconified) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    const bool min = (iconified == GLFW_TRUE);
    if (min != self->m_minimized) {
        self->m_minimized = min;
        self->update_window_mode();
        self->update_window_state();
    }
}

auto GlfwSurface::Impl::on_window_focus(GLFWwindow *w, int focused) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    const bool active = (focused == GLFW_TRUE);
    if (active != self->m_active) {
        self->m_active = active;
        self->update_window_state();
    }
}

auto GlfwSurface::Impl::on_window_maximize(GLFWwindow *w, int maximized) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    const bool max = (maximized == GLFW_TRUE);
    if (max != self->m_maximized) {
        self->m_maximized = max;
        self->update_window_mode();
    }
}

// ===== GlfwSurface 公共 API：全部委托给 m_pimpl =====
GlfwSurface::GlfwSurface(const Config &cfg) : m_pimpl(std::make_unique<Impl>(cfg)) {}
GlfwSurface::~GlfwSurface() = default;

auto GlfwSurface::set_event_handler(const EventHandler &h) -> void { m_pimpl->m_handler = h; }
auto GlfwSurface::set_window_state_handler(WindowStateHandler h) -> void {
    m_pimpl->m_window_state_handler = std::move(h);
}
auto GlfwSurface::set_window_mode_handler(WindowModeHandler h) -> void {
    m_pimpl->m_window_mode_handler = std::move(h);
}

[[nodiscard]] auto GlfwSurface::begin_frame(int width, int height) -> Result<bool> {
    return m_pimpl->begin_frame(width, height);
}
[[nodiscard]] auto GlfwSurface::painter() -> Painter & { return m_pimpl->painter(); }
[[nodiscard]] auto GlfwSurface::present() -> Result<bool> { return m_pimpl->present(); }
[[nodiscard]] auto GlfwSurface::size() const -> Size { return m_pimpl->size(); }
[[nodiscard]] auto GlfwSurface::scale_factor() const -> float { return m_pimpl->scale_factor(); }
[[nodiscard]] auto GlfwSurface::should_close() const -> bool { return m_pimpl->should_close(); }
auto GlfwSurface::poll_platform_events() -> void { Impl::poll_platform_events(); }
auto GlfwSurface::wait_events(double timeout_ms) -> void { m_pimpl->wait_events(timeout_ms); }
auto GlfwSurface::request_wake() -> void { Impl::request_wake(); }
[[nodiscard]] auto GlfwSurface::data() const -> const std::uint8_t * { return m_pimpl->data(); }
[[nodiscard]] auto GlfwSurface::frame_count() const -> int { return m_pimpl->frame_count(); }

auto GlfwSurface::capture_window(const std::string &path) -> Result<bool> {
#if defined(AURORA_PLATFORM_WINDOWS) && defined(AURORA_ENABLE_DEBUG)
    // Windows 上 GLFW 窗口底层是 Win32 HWND，直接复用 PrintWindow 路径抓取含非客户区画面。
    const HWND hwnd = glfwGetWin32Window(m_pimpl->m_window);
    return detail::capture_window_by_hwnd(hwnd, path);
#elif defined(AURORA_ENABLE_DEBUG)
    // 非 Windows 的 GLFW（X11/Wayland/Mac）真实窗口截图暂未实现，回落 unsupported。
    (void)path;
    return Result<bool>{ make_error(
        ErrorCode::GeneralNotSupported,
        "capture_window: not implemented for this GLFW platform (use native X11/Wayland backend)") };
#else
    (void)path;
    return Result<bool>{ make_error(ErrorCode::GeneralNotSupported,
                                    "capture_window: disabled (AURORA_ENABLE_DEBUG not enabled)") };
#endif
}

} // namespace aurora

#endif // AURORA_BACKEND_GLFW
