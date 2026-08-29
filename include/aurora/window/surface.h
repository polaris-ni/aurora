#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/render/png.h"
#include "aurora/window/title_bar_style.h" // TitleBarStyle：CSD 自绘标题栏样式值类型
#include "aurora/window/window_state.h"    // WindowState / WindowMode 及纯函数

namespace aurora {

/// @brief 窗口高级样式选项（规格 §1.8）：跨后端声明，由各 Surface 按能力映射。
/// Headless 忽略（无 OS 窗口）；Win32 映射到 WS_EX_TOPMOST / WS_POPUP / 去 WS_THICKFRAME /
/// WM_GETMINMAXINFO；GLFW 映射到对应 window hint（后续接入）。
/// 对标 Qt WindowStaysOnTopHint/FramelessWindowHint、WPF Topmost/ResizeMode。

/// @brief 窗口装饰策略（跨后端声明；各 Surface 按合成器能力映射）。
/// 决定「标题栏/边框由谁绘制、无原生装饰（如 GNOME 不支持 xdg-decoration）时如何兜底」，
/// 使窗口在缺标题栏或显式无边框时仍可移动/缩放/关闭——即「无标题栏也能正常运行」。
enum class DecorationPolicy : std::uint8_t {
    Auto,       ///< 自动：优先协商服务端装饰（SSD）；不可用时回退客户端自绘（CSD）标题栏。
    ServerSide, ///< 强制服务端装饰（KDE 原生标题栏）；不可用时回退 CSD 兜底。
    ClientSide, ///< 强制客户端自绘 CSD 标题栏（即便 compositor 支持 SSD）。
    Borderless, ///< 无标题栏：保留可拖拽缩放边框；移动靠修饰键拖拽（Super/Alt + 拖拽）。
    Frameless,  ///< 完全无装饰：应用自绘全部 UI，并经程序化 API（close/minimize/...）驱动窗口状态。
};

/// @brief 可缩放窗口边缘（begin_window_resize 参数；跨后端声明，各 Surface 按平台语义映射）。
/// 枚举值序即后端映射表的公共契约（Wayland kMap / Win32 kHt 均按下标取用），不得重排。
enum class WindowResizeEdge : std::uint8_t {
    None,        ///< 无效/哨兵：不发起缩放（下标 0，后端据此拒绝）。
    Top,         ///< 上边缘
    Bottom,      ///< 下边缘
    Left,        ///< 左边缘
    Right,       ///< 右边缘
    TopLeft,     ///< 左上角
    TopRight,    ///< 右上角
    BottomLeft,  ///< 左下角
    BottomRight, ///< 右下角
};

struct WindowStyleOptions {
    bool always_on_top = false; ///< 置顶（始终浮在普通窗口之上）
    bool frameless = false;     ///< 无边框（无标题栏/边框；自行实现拖拽/关闭）。等价于 DecorationPolicy::Frameless。
    DecorationPolicy decoration = DecorationPolicy::Auto; ///< 装饰策略（见 DecorationPolicy）。
    bool transparent = false;                             ///< 透明窗口（Win32: WS_EX_LAYERED；§1.8 实验性）
    bool resizable = true;                                ///< 可调大小（false = 固定尺寸，去最大化按钮）
    Size min_size{ .width = 0.0f, .height = 0.0f };       ///< 最小逻辑尺寸（0 = 不限）
    Size max_size{ .width = 0.0f, .height = 0.0f };       ///< 最大逻辑尺寸（0 = 不限）
    TitleBarStyle title_bar{};                            ///< CSD 自绘标题栏样式（Wayland/X11 等客户端装饰后端使用）
};

/**
 * @brief 表面抽象：平台窗口/画布的绘制目标（架构 §4.5 `Surface`，<<platform impl>>）。
 *
 * 一帧生命周期：`beginFrame` → 取 `painter()` 绘制 → `present` 呈现。
 * 已实现后端：`HeadlessSurface`（内存帧缓冲 + 可选 PNG 落盘）、`Win32Surface`（Win32/GDI，仅
 * `AURORA_PLATFORM_WINDOWS`）、 `GlfwSurface`（GLFW/OpenGL，跨平台），均由 `auto_detect_surface()` 按平台选择（见
 * `native_surfaces.h`）； `D3D11Surface` 经 `create_window(D3D11Options)` 显式开启。widget 层与渲染后端解耦（§4.4）。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
class Surface {
  public:
    Surface() = default;
    virtual ~Surface() = default;

    Surface(const Surface &) = delete;
    auto operator=(const Surface &) -> Surface & = delete;
    Surface(Surface &&) = delete;
    auto operator=(Surface &&) -> Surface & = delete;

    /// @brief 分配/重置一帧画布（设备像素宽高）。
    [[nodiscard]] virtual auto begin_frame(int width, int height) -> Result<bool> = 0;
    /// @brief 取当前帧绘制器（绘制目标）。
    [[nodiscard]] virtual auto painter() -> Painter & = 0;
    /// @brief 呈现当前帧（刷新到窗口/落盘）。
    [[nodiscard]] virtual auto present() -> Result<bool> = 0;
    /// @brief 当前表面尺寸（设备像素）。
    [[nodiscard]] virtual auto size() const -> Size = 0;
    /// @brief 设备像素密度（dpi / 160）。
    [[nodiscard]] virtual auto scale_factor() const -> float { return 1.0f; }
    /// @brief 平台是否已请求关闭（帧循环据此退出）。
    [[nodiscard]] virtual auto should_close() const -> bool { return false; }
    /// @brief 轮询平台原生事件（无头实现为空操作）。
    virtual auto poll_platform_events() -> void {}

    /// @brief 阻塞等待平台事件或超时。
    /// 语义：阻塞直到「有平台事件到达」或「超时」；`timeout_ms < 0` 表示无限等待
    /// （实现按上限 1000ms 分段兜底，防止丢唤醒后死等）；`timeout_ms == 0` 立即返回。
    /// 默认实现为限频 sleep（不支持阻塞等待的自定义后端至少不再忙轮询）；
    /// `Headless` 覆盖为 no-op（测试用 max_frames 有限循环驱动，不得引入等待）；
    /// Win32 经 `MsgWaitForMultipleObjectsEx`、GLFW 经 `glfwWaitEventsTimeout` 实现真阻塞。
    virtual auto wait_events(double timeout_ms) -> void {
        if (timeout_ms == 0.0) {
            return;
        }
        // 无限等待兜底为 1000ms 分段：默认实现无唤醒通道，不可真正无限睡。
        const double capped = (timeout_ms < 0.0) ? 1000.0 : std::min(timeout_ms, 1000.0);
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(capped));
    }

    /// @brief 跨线程唤醒主循环（线程安全）：使阻塞在 `wait_events` 的帧循环立即返回。
    /// 供后台线程（如 `au::async` 回投主线程）在投递工作后调用；默认空实现
    /// （默认 `wait_events` 为分段 sleep，最迟 1000ms 自然醒，不丢事件仅延迟）。
    virtual auto request_wake() -> void {}

    /// @brief 后端是否自带帧节拍（如 D3D11 vsync `Present(1,0)` 阻塞到 vblank）。
    /// 帧调度决策据此在活跃帧跳过 CPU 端 sleep 节流，避免双重限速；默认 false。
    [[nodiscard]] virtual auto paces_frames() const -> bool { return false; }

    /// @brief 事件上抛：后端把原生事件翻译为 aurora::Event 后回调（默认空实现）。
    /// 所有真实后端 override 本方法；由 Application 经 EventDispatcher + FocusManager 统一派发。
    using EventHandler = std::function<void(Event &)>;
    virtual auto set_event_handler(const EventHandler & /*h*/) -> void {}

    /// @brief 窗口可见性状态上报句柄（最小化/被遮挡/前台激活，见 `WindowState`）。
    /// 与 widget `Event` 管道正交：窗口级可见性不进入 `Application::dispatch` 命中冒泡，
    /// 由 `Application` 直接聚合为响应式 `State`。
    using WindowStateHandler = std::function<void(WindowState)>;
    /// @brief 窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen，见 `WindowMode`）。
    using WindowModeHandler = std::function<void(WindowMode)>;

    /// @brief 注册窗口可见性状态上报句柄（默认存入 `m_window_state_handler`；
    /// 真实后端可覆盖以叠加本地状态记录）。
    virtual auto set_window_state_handler(WindowStateHandler h) -> void { m_window_state_handler = std::move(h); }
    /// @brief 注册窗口几何态上报句柄（默认存入 `m_window_mode_handler`；
    /// 真实后端可覆盖以叠加本地状态记录）。
    virtual auto set_window_mode_handler(WindowModeHandler h) -> void { m_window_mode_handler = std::move(h); }

    /// @brief 立即重绘请求回调。
    /// 后端在窗口几何变化/系统要求重绘（如 Win32 `WM_SIZE`/`WM_PAINT`）时调用，
    /// 由 `Window` 接为「对当前缓存根再渲染一帧」，使帧缓冲在 DWM 合成前已为新尺寸内容，
    /// 从根源消除最大化白闪。`Headless`/`GLFW` 不调用该回调（默认空实现），行为不变。
    using PresentRequest = std::function<void()>;
    virtual auto set_present_request(PresentRequest h) -> void { m_present_request = std::move(h); }

    /// @brief 增量上屏脏区（设备坐标）：`Window` 在 present 前、脏追踪 clear 前调用，
    /// 供支持增量上传的后端（如 D3D11）仅更新变化区；默认空实现（GDI 全量 blit 忽略）。
    virtual auto set_present_dirty(const std::vector<Rect> & /*device_rects*/) -> void {}

    /// @brief 本后端 `begin_frame` 为整帧建立的底色（clear color）。
    /// 脏区裁剪重绘（`Window::present_root` 部分脏路径）会跳过 `begin_frame` 以保留上帧缓冲，
    /// 仅对脏区先 `clear_rect` 归零、再以本色重铺，使脏区底色与整帧 `begin_frame` 逐位一致；
    /// 否则脏区内无不透明背景的控件（如裸 `Text`、无背景的 `LazyList` 子项）会露出零基底（黑）。
    /// 默认透明（0,0,0,0），对应 `begin_frame` 仅 `painter().begin()` 不铺底色的后端
    /// （Headless/D3D11）；铺浅色底的窗口后端（Win32/GLFW/X11/Wayland）覆盖返回其底色。
    [[nodiscard]] virtual auto clear_color() const -> Color { return Color{ 0, 0, 0, 0 }; }

    /// @brief 运行时更新窗口标题（默认空实现；Win32 后端经 SetWindowText 生效，Headless/GLFW 忽略）。
    virtual auto set_title(const std::string & /*title*/) -> void {}

    /// @brief 运行期更新 CSD 自绘标题栏样式（默认空实现；Wayland 等客户端装饰后端覆写生效）。
    virtual auto set_title_bar_style(const TitleBarStyle & /*style*/) -> void {}

    /// @brief 运行期更新 CSD 标题栏图标（默认空实现）。参数取 `std::shared_ptr<Image>`：
    /// `Image` 内嵌整幅像素缓冲（`std::vector<std::uint8_t>`，见 core/image.h）非轻拷贝，
    /// 共享指针避免按值深拷贝像素或悬挂引用。
    virtual auto set_title_bar_icon(const std::shared_ptr<Image> & /*icon*/) -> void {}

    /// @brief 客户端装饰预留给应用内容的安全区内边距（逻辑 dp）：CSD 标题栏/边框占用的区域。
    /// 默认 0（无装饰）；Wayland CSD 下返回标题栏高度（顶）与边框厚度（四周）。
    /// `MediaQuery::from_surface` 会将其并入 `padding`，子树据此自动避开装饰（对齐 Flutter SafeArea 范式）。
    [[nodiscard]] virtual auto content_inset() const -> EdgeInsets { return EdgeInsets{}; }

    /// @brief 程序化窗口控制：请求关闭（默认空实现；Wayland 经 xdg_toplevel 生效）。
    virtual auto close() -> void {}
    /// @brief 程序化窗口控制：最小化（默认空实现）。
    virtual auto minimize() -> void {}
    /// @brief 程序化窗口控制：切换最大化（默认空实现）。
    virtual auto toggle_maximize() -> void {}
    /// @brief 程序化窗口控制：设置全屏（默认空实现）。
    virtual auto set_fullscreen(bool /*on*/) -> void {}

    /// @brief 控件发起窗口拖拽移动（默认空实现）。
    /// 须在指针按下事件派发栈内同步调用——Wayland serial 时效约束：
    /// 合成器校验调用所附 serial 必须是「最近一次」按键 serial，异步延迟调用会被拒绝。
    virtual auto begin_window_move() -> void {}
    /// @brief 控件发起窗口边缘缩放（默认空实现；serial 时效约束同 begin_window_move）。
    virtual auto begin_window_resize(WindowResizeEdge /*edge*/) -> void {}

    /// @brief 当前帧像素（仅供测试/抓帧；无缓冲返回 nullptr）。
    [[nodiscard]] virtual auto data() const -> const std::uint8_t * { return nullptr; }

    /// @brief 帧缓冲物理像素尺寸（用于 `save_snapshot` 写出 PNG 的真实宽高）。
    /// 默认等于逻辑 `size()`；当 painter 缓冲按 DPI 物理分辨率分配（Win32/D3D11）时，
    /// 后端须覆写返回物理像素，否则 PNG 宽高与像素数据错位（缩放比≠1 时图像被压扁/错位）。
    [[nodiscard]] virtual auto framebuffer_size() const -> Size { return size(); }

    /// @brief 导出当前帧软件帧缓冲为 PNG（RGBA）。
    /// 默认实现：`data()` 非空则写 PNG，否则返回 unsupported 错误（如 Release 下后端未覆写 `data()`）。
    /// 各真实后端在 `AURORA_ENABLE_DEBUG` 下覆写 `data()` 返回 Painter 缓冲后，本默认实现即通用可用，
    /// 无需逐后端重写。本方法**始终声明**（vtable 槽稳定，属 Surface 契约）；Release 下默认实现仍编译，
    /// 但仅判空返回，不引入后端专属截图代码，零开销。判空走运行时而非宏，避免宏不一致引发 ODR。
    [[nodiscard]] virtual auto save_snapshot(const std::string &path) -> Result<bool> {
        const std::uint8_t *px = data();
        if (px == nullptr) {
            return Result<bool>{ make_error(
                ErrorCode::GeneralNotSupported,
                "save_snapshot: Surface::data() returned nullptr (framebuffer capture unavailable)") };
        }
        const auto sz = framebuffer_size();
        const auto r = write_png(path.c_str(), static_cast<int>(sz.width), static_cast<int>(sz.height), px);
        if (!r) {
            return Result<bool>{ r.error() };
        }
        return Result<bool>{ true };
    }

    /// @brief 导出真实屏幕窗口为 PNG（含 OS 装饰，尽力）。
    /// 默认实现：返回 unsupported 错误。Win32/X11/GLFW 在 `AURORA_ENABLE_DEBUG` + 对应后端下覆写；
    /// Headless/Wayland 保持 unsupported（Wayland 客户端无法截图，属安全限制）。本方法**始终声明**。
    [[nodiscard]] virtual auto capture_window(const std::string &path) -> Result<bool> {
        (void)path;
        return Result<bool>{ make_error(
            ErrorCode::GeneralNotSupported,
            "capture_window: not supported on this backend (only Win32/X11/GLFW provide OS window capture)") };
    }

    /// @brief 已呈现帧数（诊断/测试用；默认 0）。
    [[nodiscard]] virtual auto frame_count() const -> int { return 0; }

    /// @brief 原生窗口句柄（平台相关；Headless/未知后端返回 nullptr）。
    /// 用于跨模块窗口操作（如多显示器窗口迁移）。默认空实现，由具体后端覆盖。
    /// 为只读查询，声明为 const（不修改 Surface 状态）。
    [[nodiscard]] virtual auto native_handle() const -> void * { return nullptr; }

  protected:
    /// @brief 上报当前窗口可见性状态（由真实后端在状态变化时调用）。
    auto notify_window_state(WindowState s) const -> void {
        if (m_window_state_handler) {
            m_window_state_handler(s);
        }
    }
    /// @brief 上报当前窗口几何态（由真实后端在几何态变化时调用）。
    auto notify_window_mode(WindowMode m) const -> void {
        if (m_window_mode_handler) {
            m_window_mode_handler(m);
        }
    }

    WindowStateHandler m_window_state_handler; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
                                               ///< 窗口可见性状态上报句柄（子类经 set_* 注册）。
    WindowModeHandler m_window_mode_handler;   // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
                                               ///< 窗口几何态上报句柄（子类经 set_* 注册）。
    PresentRequest m_present_request;          // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
                                               ///< 立即重绘请求（子类在几何变化/WM_PAINT 时调用；默认空）。
};

#ifdef AURORA_BACKEND_HEADLESS
/**
 * @brief 无头表面：在内存 `Painter` 帧缓冲上绘制，`present()` 时可写 PNG（§5.7）。
 *
 * 用于无窗口系统的单元测试与无头校验；不依赖 GLFW/SDL/OpenGL。
 * 仅当 `AURORA_BACKEND_HEADLESS` 定义（默认 ON，可由 CMake `AURORA_BACKEND_HEADLESS=OFF` 剪裁）时提供。
 */
class HeadlessSurface : public Surface {
  public:
    /// @param png_path 非空时每次 `present()` 写出 PNG（覆盖同名文件）。
    /// @param size 初始逻辑尺寸；传入后可让 `size()` 在 `begin_frame` 之前即返回正确值，
    /// 避免 `Window::present_root` 首次读取到 0 尺寸把整棵树布局到 0×0 而白屏（HEADLESS 后端特有）。
    explicit HeadlessSurface(std::string png_path = "", Size size = Size{ .width = 0.0f, .height = 0.0f })
        : m_png_path(std::move(png_path)), m_size(size) {}

    /// @brief 设置/更换 PNG 输出路径（空串表示仅留在内存）。
    auto set_png_path(std::string png_path) -> void { m_png_path = std::move(png_path); }
    /// @brief 已呈现帧数（测试与诊断用）。
    [[nodiscard]] auto frame_count() const -> int override { return m_frame; }

    /// @brief 无头后端不等待（no-op）：测试以 `max_frames` 有限循环驱动，
    /// 引入等待会拖慢 ctest 且破坏确定性；行为与历史完全一致。
    auto wait_events(double /*timeout_ms*/) -> void override {}

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override {
        m_painter.begin(width, height);
        m_size = Size{ .width = static_cast<float>(width), .height = static_cast<float>(height) };
        return Result<bool>{ true };
    }
    [[nodiscard]] auto painter() -> Painter & override { return m_painter; }
    [[nodiscard]] auto present() -> Result<bool> override {
        ++m_frame;
        if (!m_png_path.empty()) {
            auto r = write_png(m_png_path.c_str(), m_painter.width(), m_painter.height(), m_painter.data());
            if (!r) {
                return r;
            }
        }
        return Result<bool>{ true };
    }
    [[nodiscard]] auto size() const -> Size override { return m_size; }
    [[nodiscard]] auto data() const -> const std::uint8_t * override { return m_painter.data(); }

    /// @brief 测试 seams：在无 OS 窗口下确定性驱动窗口级状态（供 `test_window_state` 使用）。
    auto simulate_window_state(WindowState s) const -> void { notify_window_state(s); }
    /// @brief 测试 seams：在无 OS 窗口下确定性驱动窗口几何态。
    auto simulate_window_mode(WindowMode m) const -> void { notify_window_mode(m); }
    /// @brief 测试 seams：模拟系统重绘请求（如 Win32 最小化还原后的 WM_PAINT），
    /// 触发 `Window` 接线的同步重渲染回调；未接线时 no-op。
    auto simulate_present_request() const -> void {
        if (m_present_request) {
            m_present_request();
        }
    }

  private:
    Painter m_painter;
    // 声明顺序须与构造函数初始化列表一致（m_png_path 先于 m_size），否则触发 -Wreorder。
    std::string m_png_path;
    Size m_size{ .width = 0.0f, .height = 0.0f };
    int m_frame = 0;
};
#endif // AURORA_BACKEND_HEADLESS

} // namespace aurora
