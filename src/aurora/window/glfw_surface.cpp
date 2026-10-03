#include "aurora/window/glfw_surface.h"

#ifdef AURORA_BACKEND_GLFW

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
// Windows SDK 的 <GL/gl.h> 非自洽：函数声明使用的 WINGDIAPI/APIENTRY 由 windef.h 先行
// 定义，缺 windows.h 时新版 SDK（10.0.26100）在 MSVC 下整片解析失败。
// clang-format off
#include <windows.h>
#include <GL/gl.h>
// clang-format on
#elif defined(AURORA_PLATFORM_MACOS)
// macOS 无 <GL/gl.h>：GL 头位于 OpenGL.framework（GL 1.1 立即模式子集仍在，本文件仅用之）。
// Apple 自 10.14 起将整个 OpenGL 标记 deprecated，须在包含前定义厂商宏消噪（宏名为厂商规定）。
#define GL_SILENCE_DEPRECATION 1  // NOLINT(*-identifier-naming)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <GLFW/glfw3.h>

// `native_handle()` 逐平台取原生句柄（Win32 HWND / macOS NSWindow* / X11 Window），
// 三者都只在 `glfw3native.h` 里声明，且各自被平台宏门控 —— 缺宏则该平台的
// `glfwGet*Window` 是未声明标识符。故此处按平台只开启自己需要的那一个，
// 不多开（如不开 GLX/EGL/Wayland）以免拖入无用的系统头。
#ifdef AURORA_PLATFORM_WINDOWS
#define GLFW_EXPOSE_NATIVE_WIN32  // NOLINT(*-identifier-naming)
#elif defined(AURORA_PLATFORM_MACOS)
#define GLFW_EXPOSE_NATIVE_COCOA  // NOLINT(*-identifier-naming)
#elif defined(AURORA_PLATFORM_LINUX)
// XWayland 下 GLFW 仍经 X11 取句柄（Wayland 原生句柄由 WaylandSurface 负责），
// 故 Linux 腿只需 X11 而非 GLFW_EXPOSE_NATIVE_WAYLAND。
#define GLFW_EXPOSE_NATIVE_X11  // NOLINT(*-identifier-naming)
#endif
#include <GLFW/glfw3native.h>

// X11 的 `<X11/X.h>`（经上面 glfw3native.h 拉入）无条件定义对象宏 `CursorShape`（值 0，
// 光标最大尺寸）与 `None`（值 0L）：前者与公共类型 `aurora::CursorShape`（core/enums.h）
// 硬碰撞 → `CursorShape shape` 被展开成 `0 shape`；后者与枚举项 `ModifierKey::None`
// 碰撞 → 报 `expected unqualified-id before numeric constant`。本项目不用这两个 Xlib
// 恒定量（光标形状经 GLFW 标准光标枚举；修饰态经上表的 GLFW 掩码），故直接解除。
// 必须在本文件后续包含 aurora 头（cursor_map.h / detail/glfw_modifiers.h）之前解除 ——
// 头文件 guard 一旦把它们解析完，事后 #undef 救不回来。
// 与 `x11_surface.cpp` 的同名处理同口径（那里还需先取 `None` 的值再 #undef）。
#if defined(AURORA_PLATFORM_LINUX)
#undef CursorShape
#undef None
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/utf8.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/render/png.h"
#include "aurora/window/cursor_map.h"
#include "aurora/window/detail/glfw_dpi.h"
#include "aurora/window/detail/glfw_keymap.h"
#include "aurora/window/detail/glfw_modifiers.h"
#include "aurora/window/win32_capture.h"
#include "aurora/window/window_state.h"

#ifdef AURORA_ENABLE_GLFW_GPU_GL
#include "aurora/render/rhi/gpu_gl_rhi.h"
#endif

namespace aurora {

// ---- 修饰键 / UTF-8 翻译（纯函数，static 成员）----
//
// 键码翻译 `from_glfw_key` 已移至可单测的内部头 `detail/glfw_keymap.h`（与 `win32_keymap.h`
// 对称，使「四后端键码一致」这条契约在 GLFW 腿上也有 CTest 断言）。

/// @brief 读 NumLock 锁定态并并入修饰位集。
///
/// GLFW 的 `GLFW_MOD_*` 掩码确有 `GLFW_MOD_NUM_LOCK`（0x0020），但它**只在开启了
/// `GLFW_LOCK_KEY_MODS` 输入模式时**才随事件上报（见 `glfw3.h` 对该掩码的说明）；本后端不
/// 依赖该模式，故按 `GLFW_KEY_NUM_LOCK` 单独查询。读法用 `glfwGetKey` 的**缓存事件态**
/// 而非按下事件：NumLock 是切换键，没有「按住」语义。
/// @param w 目标窗口。
/// @return 该位已并入的修饰位集。
[[nodiscard]] static auto with_glfw_numlock(GLFWwindow *w, ModifierKey m) -> ModifierKey {
    // GLFW 平台实现对未支持的键返回 `GLFW_KEY_UNKNOWN`；此处按「读不到 = 关」处理，
    // 不静默假报「开」。
    if (glfwGetKey(w, GLFW_KEY_NUM_LOCK) == GLFW_PRESS) {
        return m | ModifierKey::NumLock;
    }
    return m;
}

// pimpl：全部 GLFW/OpenGL 状态与回调都在这里；公共头仅持有 unique_ptr<Impl>。
struct GlfwSurface::Impl {
    GLFWwindow *window = nullptr;
    /// 逻辑尺寸（dp）——对外 `size()` 的值，由帧缓冲物理像素除以 `scale` 得。
    Size logical_size{.width = 800.0F, .height = 600.0F};
    /// 内容缩放因子（`glfwGetWindowContentScale`）：GLFW 屏幕坐标 → aurora 逻辑 dp 的换算因子。
    float scale = 1.0F;
    Painter painter_impl;
    int frame = 0;
    EventHandler handler;
    bool minimized = false;  ///< 是否最小化（GLFW iconify 回调）。
    bool active = true;  ///< 是否前台激活（GLFW focus 回调）。初值 true：创建即激活。
    bool maximized = false;  ///< 是否最大化（GLFW maximize 回调）。
    WindowMode mode = WindowMode::Normal;  ///< 当前几何态（变化时上报）。
    WindowState state = WindowState::Visible;  ///< 当前可见性状态（变化时上报）。

    // ---- GL 资源（OpenGL 1.1 立即模式，仅需纹理对象）----
    bool gl_ready = false;
    GLuint tex = 0;
    int tex_w = 0;
    int tex_h = 0;

#ifdef AURORA_ENABLE_GLFW_GPU_GL
    // ---- GPU 栅格（DisplayList → OpenGL 3.3 core 批渲染；非空 = GPU 模式生效）----
    // 初始化失败（函数表缺项/着色器链接失败/上下文过老）即置空回退软件纹理路径。
    std::unique_ptr<rhi::GpuGlRhi> gpu;
    /// DEBUG 抓帧缓存：data() 首次访问时懒读回（present 置失效），未消费即零 GPU→CPU 停顿；
    /// Release 恒空。mutable：data() 为 const 而读回时机由访问触发。
    mutable std::vector<std::uint8_t> gpu_readback;
    mutable bool gpu_readback_fresh = false;
#endif

    WindowStateHandler window_state_handler;
    WindowModeHandler window_mode_handler;
    /// DPI 缩放变化上报（`glfwSetWindowContentScaleCallback` 驱动）。与上面两个 handler
    /// 同形态：Impl 自持副本，由 `GlfwSurface::set_scale_change_handler` 转存。
    ScaleChangeHandler scale_change_handler;

    // ---- 光标形状：标准光标句柄按 CursorShape 取值序缓存（nullptr = 未创建/不可用）----
    // 复用句柄而非每次 glfwCreateStandardCursor：后者每次创建都是新资源，反复悬停切换必泄漏。
    std::array<GLFWcursor *, AURORA_CURSOR_SHAPE_COUNT> cursors{};
    /// @brief 取（惰性创建）该形状的标准光标句柄；GLFW 无对应形状时返回 nullptr（调用方回退 Arrow）。
    auto cursor_for(CursorShape shape) -> GLFWcursor *;

    explicit Impl(const Config &cfg);
    ~Impl();

    Impl(const Impl &) = delete;
    auto operator=(const Impl &) -> Impl & = delete;
    Impl(Impl &&) = delete;
    auto operator=(Impl &&) -> Impl & = delete;

    auto begin_frame(int width, int height) -> Result<bool>;
    auto painter() -> Painter & { return painter_impl; }
    auto present() -> Result<bool>;
    [[nodiscard]] auto size() const -> Size { return logical_size; }
    /// 帧缓冲**物理**像素尺寸：与 `data()` 返回的软件缓冲严格同尺寸（`Surface` 契约要求
    /// painter 按物理分辨率分配时必须覆写，否则快照 PNG 宽高与像素数据错位）。
    [[nodiscard]] auto framebuffer_size() const -> Size {
        return Size{.width = static_cast<float>(painter_impl.width()),
                    .height = static_cast<float>(painter_impl.height())};
    }
    [[nodiscard]] auto scale_factor() const -> float { return scale; }
    [[nodiscard]] auto should_close() const -> bool { return glfwWindowShouldClose(window) == GLFW_TRUE; }
    static auto poll_platform_events() -> void { glfwPollEvents(); }
    auto wait_events(double timeout_ms) const -> void;
    static auto request_wake() -> void { glfwPostEmptyEvent(); }
    [[nodiscard]] auto data() const -> const std::uint8_t * {
#ifdef AURORA_ENABLE_GLFW_GPU_GL
        if (gpu != nullptr) {
            // GPU 模式：像素在显存，经 DEBUG 抓帧缓存读回；懒读回——本帧首次访问才执行
            // （present 置失效），无消费者时零全屏 GPU→CPU 读回停顿。Release 恒空 → nullptr。
#ifdef AURORA_ENABLE_DEBUG
            if (!gpu_readback_fresh) {
                gpu_readback_fresh = gpu->read_pixels(gpu_readback);
            }
            return gpu_readback_fresh && !gpu_readback.empty() ? gpu_readback.data() : nullptr;
#else
            return nullptr;
#endif
        }
#endif
        return painter_impl.data();
    }
    [[nodiscard]] auto frame_count() const -> int { return frame; }
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * {
#ifdef AURORA_ENABLE_GLFW_GPU_GL
        return gpu.get();
#else
        return nullptr;
#endif
    }

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
    static auto on_content_scale(GLFWwindow *w, float xscale, float yscale) -> void;

    /// @brief 坐标换算：GLFW 光标位置是**屏幕坐标**（DPI 感知进程里即物理像素），
    /// 换算成 aurora 逻辑 dp 须除以内容缩放因子。滚轮增量是**delta**，不换算。
    [[nodiscard]] static auto to_logical(double x, double y, float scale) -> Point {
        return Point{.x = static_cast<float>(detail::glfw_dp_from_px(static_cast<int>(x), scale)),
                     .y = static_cast<float>(detail::glfw_dp_from_px(static_cast<int>(y), scale))};
    }

    /// @brief 由最小化/最大化标志重算几何态，仅实际改变时上报。
    auto update_window_mode() -> void {
        const WindowMode want = compute_window_mode(minimized, maximized, false);
        if (want != mode) {
            mode = want;
            if (window_mode_handler) {
                window_mode_handler(want);
            }
        }
    }

    /// @brief 由最小化/激活标志重算可见性状态，仅实际改变时上报。
    auto update_window_state() -> void {
        const WindowState want = compute_window_state(minimized, active);
        if (want != state) {
            state = want;
            if (window_state_handler) {
                window_state_handler(want);
            }
        }
    }
};

GlfwSurface::Impl::Impl(const Config &cfg) {
    if (glfwInit() == 0) {
        throw std::runtime_error("GlfwSurface: glfwInit failed");
    }
    bool want_gpu = false;
#ifdef AURORA_ENABLE_GLFW_GPU_GL
    want_gpu = cfg.render_mode == RenderMode::HardwareGL;
#else
    if (cfg.render_mode == RenderMode::HardwareGL) {
        AURORA_LOG_WARN("gpu-gl",
                        "HardwareGL render mode requested but built without"
                        " AURORA_ENABLE_GLFW_GPU_GL; using software texture path");
    }
#endif
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, cfg.gl_major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, cfg.gl_minor);
    // GPU 模式请求 core profile（GLSL 管线需要；软件模式维持兼容剖面走 1.1 立即模式）。
    glfwWindowHint(GLFW_OPENGL_PROFILE, want_gpu ? GLFW_OPENGL_CORE_PROFILE : GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, cfg.resizable ? GLFW_TRUE : GLFW_FALSE);
    // 可见性策略：构造期 hint 定档（在 glfwCreateWindow 之前生效，GPU core profile 失败后的
    // 重建沿用同一 hint，无需重复设置）。Normal 档不设任何 hint，保持 GLFW 默认行为不变。
    switch (cfg.visibility) {
        case WindowVisibility::Hidden:
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // 不显示：窗口不进入用户视野
            break;
        case WindowVisibility::NoActivate:
            glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
            glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);  // 可见但不抢焦点
            break;
        case WindowVisibility::Normal:
            break;
    }

    // 建窗尺寸的单位是**屏幕坐标**（= DPI 感知进程里的物理像素），而 cfg.size 是逻辑 dp，
    // 两者只差一个内容缩放因子。GLFW 没有「尚未创建的窗口」的内容缩放查询，只能先按主显示器
    // 预估：命中主显示器时首次出现即正确（无尺寸跳变），窗口最终落到别的显示器时由下面的
    // 建窗后校正兜一次。
    float hint_scale = 1.0F;
    if (GLFWmonitor *primary = glfwGetPrimaryMonitor(); primary != nullptr) {
        float mx = 1.0F;
        float my = 1.0F;
        glfwGetMonitorContentScale(primary, &mx, &my);
        hint_scale = mx;  // 假设各向同性 DPI（与建窗后读 window content scale 同口径）
    }
    const int create_w = detail::glfw_px_from_dp(static_cast<int>(std::lround(cfg.size.width)), hint_scale);
    const int create_h = detail::glfw_px_from_dp(static_cast<int>(std::lround(cfg.size.height)), hint_scale);

    window = glfwCreateWindow(create_w, create_h, cfg.title.c_str(), nullptr, nullptr);
    if (window == nullptr && want_gpu) {
        // core profile 创建失败（驱动过老/远程桌面/虚拟机等）：降级软件模式重建窗口，不整体失败。
        AURORA_LOG_INFO("gpu-gl", "core-profile window creation failed; retrying with software compat profile");
        // 本行在 AURORA_ENABLE_GLFW_GPU_GL 关闭的配置下看似死存储（其后续唯一读点在 #ifdef 内），
        // 但它是降级路径对 want_gpu 的如实更新：GPU_GL 开启时该读点决定要不要装载 GL 函数表，删不得。
        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): 死存储只在 GPU_GL=OFF 配置下成立，开启后读点要用它
        want_gpu = false;
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
        window = glfwCreateWindow(create_w, create_h, cfg.title.c_str(), nullptr, nullptr);
    }
    if (window == nullptr) {
        glfwTerminate();
        throw std::runtime_error("GlfwSurface: glfwCreateWindow failed");
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // 启用 VSync（帧循环调度，见 specification/06-app-platform.md §3.1）

#ifdef AURORA_ENABLE_GLFW_GPU_GL
    if (want_gpu) {
        // 装载 GL 3.3 core 函数表（经 glfwGetProcAddress）并初始化 GPU 栅格后端；
        // 失败（函数表缺项/着色器链接失败/GL 错误）→ gpu 置空，软件纹理路径兜底。
        rhi::GLFn fn = rhi::load_gl(reinterpret_cast<void *(*)(const char *)>(&glfwGetProcAddress));
        gpu = std::make_unique<rhi::GpuGlRhi>(fn);
        if (!gpu->valid()) {
            AURORA_LOG_INFO("gpu-gl", "GPU raster backend unavailable; falling back to software texture path");
            gpu.reset();
        }
    }
#endif

    // 转发 GLFW 回调到本实例（ARCHITECTURE.md §3.1 事件来源）。用户指针存 Impl*，回调据此取回。
    glfwSetWindowUserPointer(window, this);
    glfwSetCursorPosCallback(window, &Impl::on_cursor_pos);
    glfwSetMouseButtonCallback(window, &Impl::on_mouse_button);
    glfwSetKeyCallback(window, &Impl::on_key);
    glfwSetScrollCallback(window, &Impl::on_scroll);
    glfwSetCharCallback(window, &Impl::on_char);
    glfwSetWindowSizeCallback(window, &Impl::on_window_size);
    glfwSetWindowIconifyCallback(window, &Impl::on_window_iconify);
    glfwSetWindowFocusCallback(window, &Impl::on_window_focus);
    glfwSetWindowMaximizeCallback(window, &Impl::on_window_maximize);
    glfwSetWindowContentScaleCallback(window, &Impl::on_content_scale);

    float xscale = 1.0F;
    float yscale = 1.0F;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    scale = xscale;  // 假设各向同性 DPI
    // 建窗后校正一次：窗口的真实内容缩放（per-monitor）可能与上面按主显示器预估的不同
    // （窗口落到了另一块屏、或主屏本身在 GLFW 初始化后才确定缩放）。校正只做一次，且在
    // 第一次 present 之前完成——与 Win32 建窗期 DPI 那条腿（08-tooling.md §8.2）同一口径。
    const int want_w = detail::glfw_px_from_dp(static_cast<int>(std::lround(cfg.size.width)), scale);
    const int want_h = detail::glfw_px_from_dp(static_cast<int>(std::lround(cfg.size.height)), scale);
    int cur_w = 0;
    int cur_h = 0;
    glfwGetWindowSize(window, &cur_w, &cur_h);
    if ((cur_w != want_w) || (cur_h != want_h)) {
        glfwSetWindowSize(window, want_w, want_h);
    }
}

GlfwSurface::Impl::~Impl() {
    for (GLFWcursor *c : cursors) {
        if (c != nullptr) {
            glfwDestroyCursor(c);
        }
    }
    if (window != nullptr) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();
}

// ---- 光标形状：CursorShape → GLFW 标准光标常量 ----

namespace {

/// @brief `CursorShape` → GLFW 标准光标常量；GLFW 无对应形状时返回 -1（调用方回退 Arrow）。
/// resize/NOT_ALLOWED 系列自 GLFW 3.4 起提供，按宏存在性守卫（3.3 上回退 Arrow）。
constexpr auto glfw_standard_cursor(CursorShape shape) -> int {
    switch (shape) {
        case CursorShape::Arrow:
            return GLFW_ARROW_CURSOR;
        case CursorShape::IBeam:
            return GLFW_IBEAM_CURSOR;
        case CursorShape::PointingHand:
            return GLFW_HAND_CURSOR;
        case CursorShape::ResizeNS:
            return GLFW_VRESIZE_CURSOR;
        case CursorShape::ResizeEW:
            return GLFW_HRESIZE_CURSOR;
        case CursorShape::Crosshair:
            return GLFW_CROSSHAIR_CURSOR;
        case CursorShape::ResizeNWSE:
#ifdef GLFW_RESIZE_NWSE_CURSOR
            return GLFW_RESIZE_NWSE_CURSOR;
#else
            break;
#endif
        case CursorShape::ResizeNESW:
#ifdef GLFW_RESIZE_NESW_CURSOR
            return GLFW_RESIZE_NESW_CURSOR;
#else
            break;
#endif
        case CursorShape::Move:
#ifdef GLFW_RESIZE_ALL_CURSOR
            return GLFW_RESIZE_ALL_CURSOR;
#else
            break;
#endif
        case CursorShape::NotAllowed:
#ifdef GLFW_NOT_ALLOWED_CURSOR
            return GLFW_NOT_ALLOWED_CURSOR;
#else
            break;
#endif
        case CursorShape::Wait:
            break;  // GLFW 无 busy/wait 标准形状：回退 Arrow。
    }
    return -1;
}

}  // namespace

auto GlfwSurface::Impl::cursor_for(CursorShape shape) -> GLFWcursor * {
    const auto idx = static_cast<std::size_t>(shape);
    if (idx >= cursors.size()) {
        return nullptr;
    }
    if (cursors.at(idx) == nullptr) {
        const int id = glfw_standard_cursor(shape);
        if (id < 0) {
            return nullptr;
        }
        cursors.at(idx) = glfwCreateStandardCursor(id);
    }
    return cursors.at(idx);
}

auto GlfwSurface::set_cursor(CursorShape shape) -> void {
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return;  // 窗口未建成（初始化失败）：安全 no-op。
    }
    GLFWcursor *handle = pimpl_->cursor_for(shape);
    if (handle == nullptr) {
        handle = pimpl_->cursor_for(CursorShape::Arrow);  // 无标准形状 → 回退默认箭头。
    }
    glfwSetCursor(pimpl_->window, handle);
}

auto GlfwSurface::Impl::begin_frame(int width, int height) -> Result<bool> {
    // 软件帧缓冲按**物理**分辨率分配，几何绘制把 dp 坐标 × scale，1:1 贴窗口避免发虚——
    // 与 Win32Surface / D3D11Surface / X11 / Wayland 同一模型。落地只需一次
    // `Painter::set_scale`（`Painter::begin` 收的是逻辑 dp，内部按 scale_ 分配物理缓冲），
    // 本后端此前漏调这一句，于是 `size()` 报的是 GLFW 屏幕坐标（物理像素）而
    // `scale_factor()` 报 1.5，同一时刻对外宣称两个互斥的尺寸口径。
    painter_impl.set_scale(scale);

    // 帧缓冲物理像素是真值源（GLFW 屏幕坐标）；报不出来时（最小化等）按调用方给的
    // 逻辑 dp × scale 兜底。
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0) {
        fb_w = detail::glfw_px_from_dp(width, scale);
    }
    if (fb_h <= 0) {
        fb_h = detail::glfw_px_from_dp(height, scale);
    }
    if (fb_w <= 0) {
        fb_w = 1;
    }
    if (fb_h <= 0) {
        fb_h = 1;
    }

    // 对外逻辑尺寸（dp）= 物理像素 ÷ scale；物理像素只留在本函数内部与 framebuffer_size()。
    const int dp_w = detail::glfw_dp_from_px(fb_w, scale);
    const int dp_h = detail::glfw_dp_from_px(fb_h, scale);
    logical_size = Size{.width = static_cast<float>(dp_w), .height = static_cast<float>(dp_h)};
    // 判据用 painter 的**物理**缓冲尺寸（Painter::width/height 返回物理值），与
    // begin_frame 的入参单位无关，故两次调用间不会因单位混用而每帧重分配。
    if ((fb_w != painter_impl.width()) || (fb_h != painter_impl.height())) {
        painter_impl.begin(dp_w, dp_h);
    }

    // 每帧用浅色背景清空软件帧缓冲：默认文字为黑色，需要浅色底才能可见
    // （widget 默认 Color::black()；此前全屏纹理为透明黑导致黑底黑字不可见）。
    // 矩形给**逻辑 dp**，由 painter 内部 × scale 落到物理缓冲。
    // GPU 模式下本 fill 处于录制模式（present_root 先 record 后 begin_frame），
    // 命令入帧 DL 承担窗口底色，软件像素缓冲仅为回退兜底。
    painter_impl.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = logical_size},
                           Color{245, 245, 247, 255});

    // 默认帧缓冲清屏仅软件路径需要（立即模式全屏 quad 不覆盖区外的边角）；
    // GPU 路径 end_frame 整帧 blit 覆盖默认帧缓冲，清屏纯冗余。
#ifdef AURORA_ENABLE_GLFW_GPU_GL
    if (gpu == nullptr)
#endif
    {
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.961F, 0.961F, 0.969F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    return Result<bool>{true};
}

auto GlfwSurface::Impl::present() -> Result<bool> {
#ifdef AURORA_ENABLE_GLFW_GPU_GL
    if (gpu != nullptr) {
        // GPU 路径：栅格已在 GpuGlRhi::end_frame 内完成（blit 至默认帧缓冲），跳过 CPU 上传直接 swap。
        // 抓帧缓存置失效：data() 下次访问时懒读回（未访问即零 GPU→CPU 读回成本）。
        gpu_readback_fresh = false;
        glfwSwapBuffers(window);
        ++frame;
        return Result<bool>{true};
    }
#endif
    upload_and_draw();
    glfwSwapBuffers(window);
    ++frame;
    return Result<bool>{true};
}

auto GlfwSurface::Impl::ensure_gl_objects() -> void {
    if (gl_ready) {
        return;
    }
    // 仅创建纹理对象；顶点用立即模式绘制（见 uploadAndDraw）。
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glBindTexture(GL_TEXTURE_2D, 0);
    gl_ready = true;
}

auto GlfwSurface::Impl::upload_and_draw() -> void {
    ensure_gl_objects();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    // 纹理按 painter 的**物理**缓冲尺寸分配（Painter::width/height 返回物理像素），
    // 与全屏四边形的采样坐标逐像素对齐；上传后不缩放，故与帧缓冲 1:1 呈现。
    const int buf_w = painter_impl.width();
    const int buf_h = painter_impl.height();
    if (tex_w != buf_w || tex_h != buf_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buf_w, buf_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, painter_impl.data());
        tex_w = buf_w;
        tex_h = buf_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, buf_w, buf_h, GL_RGBA, GL_UNSIGNED_BYTE, painter_impl.data());
    }
    // 立即模式全屏四边形：UV 翻转使软件帧缓冲（上→下）正确呈现为直立图像。
    glBegin(GL_QUADS);
    glTexCoord2f(0.0F, 1.0F);
    glVertex2f(-1.0F, -1.0F);
    glTexCoord2f(1.0F, 1.0F);
    glVertex2f(1.0F, -1.0F);
    glTexCoord2f(1.0F, 0.0F);
    glVertex2f(1.0F, 1.0F);
    glTexCoord2f(0.0F, 0.0F);
    glVertex2f(-1.0F, 1.0F);
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
    if (self == nullptr || !self->handler) {
        return;
    }
    MouseEvent e;
    e.action = MouseAction::Move;
    e.button = MouseButton::Left;
    e.position = to_logical(x, y, self->scale);
    // 本回调签名不带 `mods`（GLFW 只给 button / key / char 回调发掩码），故读缓存事件态。
    e.modifiers = detail::glfw_cached_modifiers(w);
    self->handler(e);
}

auto GlfwSurface::Impl::on_mouse_button(GLFWwindow *w, int button, int action, int mods) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->handler) {
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
    e.position = to_logical(x, y, self->scale);
    // 回调形参已带 `mods` 掩码，直接折算——与键盘路径同一入口、同一口径。
    e.modifiers = detail::glfw_mods_to_aurora(mods);
    self->handler(e);
}

auto GlfwSurface::Impl::on_key(GLFWwindow *w, int key, int /*scancode*/, int action, int mods) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->handler) {
        return;
    }
    KeyEvent e;
    e.key = static_cast<int>(detail::from_glfw_key(key));
    e.action = (action == GLFW_RELEASE) ? KeyAction::Up : KeyAction::Down;
    e.modifiers = with_glfw_numlock(w, detail::glfw_mods_to_aurora(mods));
    self->handler(e);
}

auto GlfwSurface::Impl::on_scroll(GLFWwindow *w, double xoff, double yoff) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->handler) {
        return;
    }
    ScrollEvent e;
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    e.position = to_logical(x, y, self->scale);
    e.delta_x = static_cast<float>(xoff);
    e.delta_y = static_cast<float>(yoff);
    // 同 on_cursor_pos：本回调签名不带 `mods`，读 GLFW 缓存事件态（详见 detail/glfw_modifiers.h）。
    e.modifiers = detail::glfw_cached_modifiers(w);
    self->handler(e);
}

auto GlfwSurface::Impl::on_char(GLFWwindow *w, unsigned int codepoint) -> void {
    const auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr || !self->handler) {
        return;
    }
    TextInputEvent e;
    e.text = utf8_encode(codepoint);
    self->handler(e);
}

auto GlfwSurface::Impl::on_window_size(GLFWwindow *w, int /*width*/, int /*height*/) -> void {  // NOLINT
    // 实际尺寸在 beginFrame 中读取并应用；此处仅确保事件被消费。
    (void)w;
}

auto GlfwSurface::Impl::on_content_scale(GLFWwindow *w, float xscale, float /*yscale*/) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    // 口径与 Win32 的 `handle_dpi_changed` 一致：取「实际生效的缩放」这一个真值，
    // 变化才上报。取值优先用 GLFW 递来的 xscale（它是 WM_DPICHANGED 的 wParam 换算而来），
    // 但仍与成员里的旧值比对一次——GLFW 在 Wayland / X11 之外的个别平台上可能递来 0
    // （表示「该输出缩放未知」），此时退化到成员值不变，等同于无变化。
    const float next = (xscale > 0.0F) ? xscale : self->scale;
    if (next == self->scale) {
        return;
    }
    self->scale = next;
    // 先换算再上报：`WindowHost::on_scale_changed` 会立刻 force_full_redraw()，
    // 若上报滞后一帧则该帧会拿旧 scale 分配缓冲 → 内容错位一帧。
    if (self->scale_change_handler) {
        self->scale_change_handler(next);
    }
}

auto GlfwSurface::Impl::on_window_iconify(GLFWwindow *w, int iconified) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    const bool min = (iconified == GLFW_TRUE);
    if (min != self->minimized) {
        self->minimized = min;
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
    if (active != self->active) {
        self->active = active;
        self->update_window_state();
    }
}

auto GlfwSurface::Impl::on_window_maximize(GLFWwindow *w, int maximized) -> void {
    auto *self = static_cast<Impl *>(glfwGetWindowUserPointer(w));
    if (self == nullptr) {
        return;
    }
    const bool max = (maximized == GLFW_TRUE);
    if (max != self->maximized) {
        self->maximized = max;
        self->update_window_mode();
    }
}

// ===== GlfwSurface 公共 API：全部委托给 pimpl_ =====
GlfwSurface::GlfwSurface(const Config &cfg) : pimpl_(std::make_unique<Impl>(cfg)) {}
GlfwSurface::~GlfwSurface() = default;

auto GlfwSurface::set_event_handler(const EventHandler &h) -> void { pimpl_->handler = h; }
auto GlfwSurface::set_window_state_handler(WindowStateHandler h) -> void {
    pimpl_->window_state_handler = std::move(h);
}
auto GlfwSurface::set_window_mode_handler(WindowModeHandler h) -> void { pimpl_->window_mode_handler = std::move(h); }
auto GlfwSurface::set_scale_change_handler(ScaleChangeHandler h) -> void {
    pimpl_->scale_change_handler = std::move(h);
}

// ---- z 序（多窗口）----

auto GlfwSurface::focus_window() -> void {
    if (pimpl_ != nullptr && pimpl_->window != nullptr) {
        glfwFocusWindow(pimpl_->window);  // GLFW 标准入口：置顶 + 取键盘焦点
    }
}

auto GlfwSurface::raise() -> void {
    // GLFW 没有独立的「提升 z 序而不激活」API；`glfwShowWindow` 会把窗口带到前面，
    // 是最接近的近似（窗口已可见时为空操作）。
    if (pimpl_ != nullptr && pimpl_->window != nullptr) {
        glfwShowWindow(pimpl_->window);
    }
}

// ---- 窗口几何（多窗口：几何持久化的读写端）----

auto GlfwSurface::position() const -> Point {
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return Point{};
    }
    int x = 0;
    int y = 0;
    glfwGetWindowPos(pimpl_->window, &x, &y);
    return Point{.x = static_cast<float>(x), .y = static_cast<float>(y)};
}

auto GlfwSurface::set_position(Point p) -> void {
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return;
    }
    glfwSetWindowPos(pimpl_->window, static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)));
}

auto GlfwSurface::set_size(Size s) -> void {
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return;
    }
    // 入参是逻辑 dp，GLFW 窗口尺寸是屏幕坐标（物理像素）——必须换算，否则 ≠100% DPI 下
    // 窗口会比请求的 dp 小一个 scale 倍（与建窗路径 `glfwCreateWindow` 同一换算）。
    glfwSetWindowSize(pimpl_->window, detail::glfw_px_from_dp(static_cast<int>(std::lround(s.width)), pimpl_->scale),
                      detail::glfw_px_from_dp(static_cast<int>(std::lround(s.height)), pimpl_->scale));
}

[[nodiscard]] auto GlfwSurface::framebuffer_size() const -> Size { return pimpl_->framebuffer_size(); }

[[nodiscard]] auto GlfwSurface::begin_frame(int width, int height) -> Result<bool> {
    return pimpl_->begin_frame(width, height);
}
[[nodiscard]] auto GlfwSurface::painter() -> Painter & { return pimpl_->painter(); }
[[nodiscard]] auto GlfwSurface::present() -> Result<bool> { return pimpl_->present(); }
[[nodiscard]] auto GlfwSurface::size() const -> Size { return pimpl_->size(); }
[[nodiscard]] auto GlfwSurface::scale_factor() const -> float { return pimpl_->scale_factor(); }
[[nodiscard]] auto GlfwSurface::should_close() const -> bool { return pimpl_->should_close(); }
auto GlfwSurface::poll_platform_events() -> void { Impl::poll_platform_events(); }
auto GlfwSurface::wait_events(double timeout_ms) -> void { pimpl_->wait_events(timeout_ms); }
auto GlfwSurface::request_wake() -> void { Impl::request_wake(); }
[[nodiscard]] auto GlfwSurface::data() const -> const std::uint8_t * { return pimpl_->data(); }
[[nodiscard]] auto GlfwSurface::frame_count() const -> int { return pimpl_->frame_count(); }

auto GlfwSurface::native_handle() const -> void * {
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return nullptr;
    }
    // 各平台原生句柄：Windows = HWND，macOS = NSWindow*，X11 = Window(为 XID 非指针)。
    // 与 Win32 / D3D11 / X11 / Wayland 四个后端同口径（`Surface::native_handle` 契约）。
    // 此前本后端未覆写 → 恒返回 nullptr，使 GLFW 窗口的跨屏行为在本仓**不可观测**：
    // 真机探针既拿不到句柄注入平台消息、也无处核对原生几何。
#if defined(AURORA_PLATFORM_WINDOWS)
    return reinterpret_cast<void *>(glfwGetWin32Window(pimpl_->window));  // NOLINT(*-pro-type-reinterpret-cast)
#elif defined(AURORA_PLATFORM_MACOS)
    return glfwGetCocoaWindow(pimpl_->window);
#elif defined(AURORA_PLATFORM_LINUX)
    // XWayland 下 GLFW 仍经 X11 取句柄（Wayland 原生路径由 WaylandSurface 负责）。
    return reinterpret_cast<void *>(
        static_cast<std::intptr_t>(glfwGetX11Window(pimpl_->window)));  // NOLINT(*-pro-type-reinterpret-cast)
#else
    return nullptr;  // 未知平台：无稳定句柄语义，不猜。
#endif
}
[[nodiscard]] auto GlfwSurface::gpu_backend() -> rhi::RhiFrameSink * { return pimpl_->gpu_backend(); }

auto GlfwSurface::capture_window(const std::string &path) -> Result<bool> {
#if defined(AURORA_PLATFORM_WINDOWS) && defined(AURORA_ENABLE_DEBUG)
    // Windows 上 GLFW 窗口底层是 Win32 HWND，直接复用 PrintWindow 路径抓取含非客户区画面。
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: GLFW window not available")};
    }
    const HWND hwnd = glfwGetWin32Window(pimpl_->window);
    return detail::capture_window_by_hwnd(hwnd, path);
#elif defined(AURORA_ENABLE_DEBUG)
    // 非 Windows 的 GLFW（X11/Wayland/Mac）：GL 帧缓冲读回。swap 后 back buffer 内容按规范
    // 未定义，故软件路径先重放一次 upload_and_draw（绘向 back buffer，不 swap，画面无感），
    // 再 glFinish + glReadPixels 得到确定内容；GPU 路径走 GpuGlRhi::read_pixels 诊断读回。
    if (pimpl_ == nullptr || pimpl_->window == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: GLFW window not available")};
    }
    if (pimpl_->frame == 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: no frame presented yet")};
    }
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(pimpl_->window, &w, &h);
    if (w <= 0 || h <= 0) {
        return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: zero-size framebuffer")};
    }
    std::vector<std::uint8_t> gl_rows(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    // glfwGetCurrentContext 返回的是「当前上下文所属窗口」指针（GLFW ABI），据以还原。
    GLFWwindow *prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(pimpl_->window);
#ifdef AURORA_ENABLE_GLFW_GPU_GL
    if (pimpl_->gpu != nullptr) {
        const bool ok = pimpl_->gpu->read_pixels(gl_rows);
        glfwMakeContextCurrent(prev);
        if (!ok) {
            return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: GPU readback failed")};
        }
        // read_pixels 以设备尺寸 resize；与当前 framebuffer 不一致 = 末帧后窗口已缩放，拒绝错位出图。
        if (gl_rows.size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U) {
            return Result<bool>{
                make_error(ErrorCode::GeneralNotSupported, "capture_window: framebuffer resized since last present")};
        }
    } else
#endif
    {
        pimpl_->upload_and_draw();  // 重放上一帧 → back buffer 内容确定（不 swap，屏幕无变化）
        glFinish();  // 等待绘批落定后读回
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl_rows.data());
        glfwMakeContextCurrent(prev);  // 还原调用方上下文，不劫持线程状态
    }
    // 垂直翻转：GL 帧缓冲自下而上 → PNG 自上而下。
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4;
    std::vector<std::uint8_t> rgba(gl_rows.size());
    for (int y = 0; y < h; ++y) {
        const std::size_t src = static_cast<std::size_t>(h - 1 - y) * row_bytes;
        const std::size_t dst = static_cast<std::size_t>(y) * row_bytes;
        std::copy(gl_rows.begin() + static_cast<std::ptrdiff_t>(src),
                  gl_rows.begin() + static_cast<std::ptrdiff_t>(src + row_bytes),
                  rgba.begin() + static_cast<std::ptrdiff_t>(dst));
    }
    if (write_png(path.c_str(), w, h, rgba.data())) {
        return Result<bool>{true};
    }
    return Result<bool>{make_error(ErrorCode::GeneralNotSupported, "capture_window: write_png failed")};
#else
    (void)path;
    return Result<bool>{
        make_error(ErrorCode::GeneralNotSupported, "capture_window: disabled (AURORA_ENABLE_DEBUG not enabled)")};
#endif
}

}  // namespace aurora

#endif  // AURORA_BACKEND_GLFW
