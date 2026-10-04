#pragma once

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/window/surface.h"

namespace aurora {

/// @brief 真实平台 Surface 后端：基于 GLFW + OpenGL（上下文默认 3.3 兼容剖面，绘制采用 1.1 立即模式）。
///
/// 复用现有软件 `Painter` 作中间帧缓冲（栅格化），每帧把像素上传到一张 GL 纹理，
/// 再用 OpenGL 1.1 立即模式绘制全屏纹理四边形呈现。采用立即模式而非 GLSL 着色器，
/// 是因为 Windows 的 `<GL/gl.h>` 仅声明 OpenGL 1.1，GLSL 函数需额外加载器（GLAD 等）；
/// 立即模式仅需系统 OpenGL 库（Windows `opengl32` / 类 Unix `libGL`），零额外依赖，
/// 跨工具链（MSVC/MinGW/GCC）可直接编译。
/// 这把「Surface 可插拔」理念落地成一个真实后端：widget 层依旧只认识
/// `Surface`/`Painter`，不感知 GLFW/GL。
///
/// Support：
/// - 渲染：OpenGL 1.1 立即模式纹理四边形（无需着色器/VAO/GL 加载器），见 `Impl::ensure_gl_objects`/`upload_and_draw`。
/// - 事件：鼠标/键盘（含 GLFW 键码 → `KeyCode` 映射）、滚轮、文本输入、窗口 resize
/// 均翻译为 aurora `Event`，经 `set_event_handler` 暴露（ARCHITECTURE.md §3.1）。
/// - 高 DPI：内容缩放因子取自 `glfwGetWindowContentScale`。**单位口径**：GLFW 3.3 起窗口尺寸与
///   光标位置用的都是**屏幕坐标**（DPI 感知进程里即物理像素），而 aurora 的窗口模型是
///   「消费方只见逻辑 dp、物理像素只留在后端内部」，两者只差一个 content scale。故本后端
///   把 painter 按**物理**分辨率分配（`Painter::set_scale` + `begin(逻辑 dp)`，与 Win32 / D3D11 /
///   X11 / Wayland 同模型），`size()` 是 dp 而 `framebuffer_size()` 是物理像素；换算收敛在
///   内部头 `detail/glfw_dpi.h`，由 `utest_glfw_dpi` 钉住。**跨屏移动导致的缩放变化经
///   `set_scale_change_handler` 上报**（GLFW 的 content scale 回调由 `WM_DPICHANGED` 驱动）。
///
/// pimpl 封装：公共头不再包含 <GL/gl.h> / <GLFW/glfw3.h>，所有 GLFW/OpenGL 细节（窗口、
/// 纹理、键码映射、回调转发等）移入 src/aurora/window/glfw_surface.cpp 的 Impl，
/// 仅暴露 `std::unique_ptr<Impl> pimpl_`；跨平台消费者无需拉入 GLFW/GL 头。
///
/// 编译需链接 glfw3 与系统 OpenGL；无 GLFW 环境不纳入默认构建（见 CMake
/// `AURORA_BACKEND_GLFW`，由 `AURORA_BACKEND_GLFW` 开关控制，默认 OFF）。GLFW 初始化失败
/// （无显示/驱动）会抛 `std::runtime_error`，调用方需捕获。
///
class GlfwSurface : public Surface {
  public:
    /// @brief 渲染模式：软件栅格 + 全屏纹理上传（默认，全平台可用）或 GPU 栅格。
    enum class RenderMode : std::uint8_t {
        SoftwareTexture,  ///< 软件栅格，每帧 CPU 像素上传为 GL 纹理呈现（历史路径）
        HardwareGL,  ///< GPU 栅格：DisplayList 经 GpuGlRhi 渲染进 MSAA 帧缓冲；需 GPU_GL 开关，失败回退软件模式
    };

    /// @brief 后端配置。**逻辑尺寸为 aurora 坐标系下的 dp**（不含 DPI 缩放）；后端内部按
    /// 内容缩放因子换算成 GLFW 屏幕坐标（物理像素）再建窗。
    struct Config {
        Size size{.width = 800.0F, .height = 600.0F};  ///< 逻辑尺寸（dp，× scale 才是窗口物理像素）
        std::string title{"Aurora"};  ///< 窗口标题（UTF-8 字节串，交给平台窗口系统显示）
        int gl_major = 3;  ///< 请求的 OpenGL 主版本号（GLFW_CONTEXT_VERSION_MAJOR）
        int gl_minor = 3;  ///< 请求的 OpenGL 次版本号（GLFW_CONTEXT_VERSION_MINOR）
        bool resizable = true;  ///< 窗口是否允许用户缩放（glfwWindowHint(GLFW_RESIZABLE)）
        RenderMode render_mode = RenderMode::SoftwareTexture;  ///< 渲染模式（默认软件上传）。
        /// @brief 窗口可见性策略（默认 `Normal` = 行为不变）：构造期经 GLFW window hint 定档，
        /// 避免「先可见后隐藏」造成的一帧闪烁。
        WindowVisibility visibility = WindowVisibility::Normal;
    };

    /// @brief 按 cfg 创建 GLFW 窗口并初始化 GL 上下文（可见性经 window hint 构造期定档）。
    /// @param cfg 后端配置：逻辑尺寸、标题、GL 版本、可缩放、渲染模式与可见性策略。
    /// @throws std::runtime_error GLFW 初始化或窗口创建失败（无显示/驱动环境）。
    explicit GlfwSurface(const Config &cfg);
    /// @brief 销毁 GLFW 窗口、释放 GL 纹理/光标句柄等后端资源并解除 event handler 绑定。
    ~GlfwSurface() override;

    /// @brief 禁止拷贝：窗口与 pimpl 所有权唯一。
    GlfwSurface(const GlfwSurface &) = delete;
    /// @brief 禁止拷贝赋值：窗口所有权唯一，任何使用均为编译期错误。
    /// @return 删除声明无运行期返回值。
    auto operator=(const GlfwSurface &) -> GlfwSurface & = delete;
    /// @brief 禁止移动：GLFW 回调 user-data 指针绑定 this，移动会悬垂。
    GlfwSurface(GlfwSurface &&) = delete;
    /// @brief 禁止移动赋值：回调 user-data 绑定 this，任何使用均为编译期错误。
    /// @return 删除声明无运行期返回值。
    auto operator=(GlfwSurface &&) -> GlfwSurface & = delete;

    /// @brief 事件处理器：GLFW 原生事件翻译为 aurora `Event` 后上抛（ARCHITECTURE.md §3.1），由 Application 统一派发。
    /// @param h 事件接收器；为空时回调仅更新窗口状态、不上抛事件。
    auto set_event_handler(const EventHandler &h) -> void override;
    /// @brief 注册窗口可见性状态上报句柄（最小化/被遮挡/前台激活）。
    /// @param h 状态变化回调；平台无法探测时可能不触发。
    auto set_window_state_handler(WindowStateHandler h) -> void override;
    /// @brief 注册窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen）。
    /// @param h 几何态回调，由 GLFW 窗口状态与尺寸回调翻译产生。
    auto set_window_mode_handler(WindowModeHandler h) -> void override;
    /// @brief 注册 DPI 缩放变化上报句柄（跨屏拖动导致 content scale 改变时触发）。
    /// @param h 缩放回调；参数为新的 `scale_factor()` 值。仅实际变化时触发。
    /// @note 逻辑↔物理换算随之改变，框架侧收到即强制整帧重排重绘。
    auto set_scale_change_handler(ScaleChangeHandler h) -> void override;

    /// @brief 运行时更新悬停光标形状：`glfwSetCursor` + `glfwCreateStandardCursor`。
    /// 标准光标句柄按 `CursorShape` 取值序缓存在 Impl（每次重建会泄漏，故复用），析构统一释放。
    /// 后端映射：Arrow→`GLFW_ARROW_CURSOR`、IBeam→`GLFW_IBEAM_CURSOR`、PointingHand→`GLFW_HAND_CURSOR`、
    /// ResizeNS→`GLFW_VRESIZE_CURSOR`、ResizeEW→`GLFW_HRESIZE_CURSOR`、Crosshair→`GLFW_CROSSHAIR_CURSOR`、
    /// ResizeNWSE/NESW→`GLFW_RESIZE_NWSE/NESW_CURSOR`、Move→`GLFW_RESIZE_ALL_CURSOR`、
    /// NotAllowed→`GLFW_NOT_ALLOWED_CURSOR`（后四项 GLFW 3.4+，宏缺失时回退 Arrow）；
    /// Wait→GLFW 无 busy/wait 标准形状，回退 Arrow。
    /// @param shape 目标光标形状；按上述映射生效，未支持形状回退 Arrow。
    /// @note 未编译验证：本机（Linux 容器）缺 GL 与 X11 扩展开发包（`libgl-dev` /
    /// `libxrandr-dev` 等），GLFW 源码构建在 configure 阶段即中止；须在有这些开发包的环境
    /// （CI / Windows / macOS）构建后复查。
    auto set_cursor(CursorShape shape) -> void override;

    /// @brief 开始新帧：按**帧缓冲物理像素**（重）建软件栅格缓冲并铺浅色底色。
    /// @param width 期望帧宽（逻辑 dp）；GLFW 报不出帧缓冲尺寸时按 `width × scale` 兜底。
    /// @param height 期望帧高（逻辑 dp）；同上。
    /// @return 恒为成功（缓冲尺寸异常时以 `width × scale` 兜底）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 当前帧的软件栅格化 Painter（写入内部像素缓冲）。
    /// @return Painter 引用，生命周期同本 Surface。
    [[nodiscard]] auto painter() -> Painter & override;
    /// @brief 提交当前帧：软件路径把像素缓冲上传 GL 纹理并以 1.1 立即模式绘制全屏四边形；
    /// GPU 路径栅格已在 `GpuGlRhi::end_frame` 完成，直接交换缓冲。
    /// @return 呈现成功 true；窗口失效时 false 及错误信息。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 逻辑尺寸（dp）：= 帧缓冲物理像素 ÷ 内容缩放因子。
    /// @return 最近一次 begin_frame 换算出的逻辑尺寸。
    [[nodiscard]] auto size() const -> Size override;
    /// @brief 帧缓冲**物理**像素尺寸：与 `data()` 的软件缓冲严格同尺寸。
    ///
    /// 本后端的 painter 按物理分辨率分配（HiDPI 下 1 dp = scale px 绘制，避免发虚），
    /// 故必须覆写——基类默认返回逻辑 `size()`，会让 `save_snapshot` 写出的 PNG 宽高与
    /// 像素数据错位（缩放比 ≠1 时图像被压扁）。
    /// @return 物理像素尺寸；尚未 begin_frame 时为 0。
    [[nodiscard]] auto framebuffer_size() const -> Size override;
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    /// @return RGB(245,245,247) 不透明浅色。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }
    /// @brief 内容缩放因子（HiDPI）：取自 `glfwGetWindowContentScale` 的 x 向缩放（假设各向同性）。
    /// @return 窗口内容坐标与物理像素的缩放倍数；未刷新时初始 1.0。
    [[nodiscard]] auto scale_factor() const -> float override;
    /// @brief 用户是否请求关闭窗口（`glfwWindowShouldClose`）。
    /// @return 窗口收到关闭请求时 true。
    [[nodiscard]] auto should_close() const -> bool override;

    /// @brief 非阻塞处理 GLFW 事件队列（`glfwPollEvents`），回调翻译为 aurora `Event` 派发。
    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待事件或超时：`glfwWaitEventsTimeout`。
    /// 无限等待按 1s 分段兜底（防丢唤醒死等）。
    /// @param timeout_ms 等待上限毫秒；0 或已请求关闭立即返回，负值/超 1000ms 均按 1s 分段。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒：`glfwPostEmptyEvent` 使阻塞在 wait_events 的主循环立即返回（线程安全）。
    auto request_wake() -> void override;
    /// @brief GLFW 的 `glfwPollEvents()` 是进程级共享队列：一次调用处理**全部**窗口事件，
    /// 多窗口帧循环每帧只需 pump 一次。
    /// @return 恒为 true（GLFW 事件队列为进程级共享）。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return true; }
    /// @brief GLFW 的 `glfwWaitEventsTimeout` 对任意窗口事件均返回，等待通道是进程级的。
    /// @return 恒为 true（任一窗口的事件均可唤醒本等待通道）。
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return true; }

    /// @brief 激活窗口（`glfwFocusWindow`）。
    auto focus_window() -> void override;
    /// @brief 提升 z 序：GLFW 无独立 API，以 `glfwShowWindow` 近似（已可见时为空操作）。
    auto raise() -> void override;
    /// @brief 窗口屏幕位置（`glfwGetWindowPos`，物理像素）。
    /// @return 窗口左上角坐标；窗口句柄失效时返回 (0,0)。
    [[nodiscard]] auto position() const -> Point override;
    /// @brief 程序化移动窗口（`glfwSetWindowPos`）。
    /// @param p 目标左上角位置（物理像素）。
    auto set_position(Point p) -> void override;
    /// @brief 原生窗口句柄：Windows = `HWND`，macOS = `NSWindow*`，X11/XWayland = `Window`
    /// （X11 的 XID，本质是非指针整数）。窗口未就绪或平台无稳定句柄语义时返回 nullptr。
    /// @note 与 Win32 / D3D11 / X11 / Wayland 后端同口径；用于跨模块窗口操作
    ///       （注入平台消息、核对原生几何、wgpu surface 创建等）。
    [[nodiscard]] auto native_handle() const -> void * override;
    /// @brief 程序化设置窗口尺寸（内部按内容缩放因子把逻辑 dp 换算为 GLFW 屏幕坐标）。
    /// @param s 目标逻辑尺寸（dp）。
    auto set_size(Size s) -> void override;

    /// @brief 最近一帧的软件像素缓冲（RGBA8，**物理**分辨率）。
    /// @return 首像素只读指针，尺寸见 `framebuffer_size()`；GPU 模式下为懒读回缓存（可能滞后一帧）。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    /// @brief 已成功 present 的帧计数。
    /// @return 自构造以来的呈现帧数。
    [[nodiscard]] auto frame_count() const -> int override;
    /// @brief GPU 帧调度挂点：GPU 模式生效时返回 `GpuGlRhi`，否则 nullptr（软件路径/回退后）。
    /// 未编译 `AURORA_ENABLE_GLFW_GPU_GL` 时恒为 nullptr。
    /// @return 帧 sink 裸指针，所有权归 Impl；软件路径恒 nullptr。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;
    /// @brief 真实窗口截图：Windows 经 GLFW 原生 HWND 复用 PrintWindow 路径（含非客户区）；
    /// 其它平台在 `AURORA_ENABLE_DEBUG` 下走 GL 帧缓冲读回（须在某次 present 之后调用，客户区画面、
    /// framebuffer 尺寸）；未开 DEBUG 回落 unsupported。
    /// @param path 输出 PNG 文件路径。
    /// @return 写盘成功 true；窗口失效/平台不支持时 false 及错误信息。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

  private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;  ///< GLFW 窗口/GL 资源/回调状态的 pimpl 实现体（公共头不含平台头）
};

}  // namespace aurora
