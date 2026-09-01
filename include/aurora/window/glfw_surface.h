#pragma once

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/window/surface.h"

namespace aurora {

/**
 * @brief 真实平台 Surface 后端：基于 GLFW + OpenGL（上下文默认 3.3 兼容剖面，绘制采用 1.1 立即模式）。
 *
 * 复用现有软件 `Painter` 作中间帧缓冲（栅格化），每帧把像素上传到一张 GL 纹理，
 * 再用 OpenGL 1.1 立即模式绘制全屏纹理四边形呈现。采用立即模式而非 GLSL 着色器，
 * 是因为 Windows 的 `<GL/gl.h>` 仅声明 OpenGL 1.1，GLSL 函数需额外加载器（GLAD 等）；
 * 立即模式仅需系统 `opengl32`，零额外依赖，跨工具链（MSVC/MinGW）可直接编译。
 * 这把「Surface 可插拔」理念落地成一个真实后端：widget 层依旧只认识
 * `Surface`/`Painter`，不感知 GLFW/GL。
 *
 * 已落地：
 *  - 渲染：OpenGL 1.1 立即模式纹理四边形（无需着色器/VAO/GL 加载器），见 `Impl::ensure_gl_objects`/`upload_and_draw`。
 *  - 事件：鼠标/键盘（含 GLFW 键码 → `KeyCode` 映射）、滚轮、文本输入、窗口 resize
 *    均翻译为 aurora `Event`，经 `set_event_handler` 暴露（ARCHITECTURE.md §3.1）。
 *  - 高 DPI：用 `glfwGetWindowContentScale` 取 scaleFactor；坐标换算采用「内容坐标即
 *    aurora 逻辑坐标」模型（GLFW 光标位置本就是内容坐标，与 widget 布局空间一致）。
 *
 * pimpl 封装：公共头不再包含 <GL/gl.h> / <GLFW/glfw3.h>，所有 GLFW/OpenGL 细节（窗口、
 * 纹理、键码映射、回调转发等）移入 src/aurora/window/glfw_surface.cpp 的 Impl，
 * 仅暴露 `std::unique_ptr<Impl> m_pimpl`；跨平台消费者无需拉入 GLFW/GL 头。
 *
 * 编译需链接 glfw3 与系统 OpenGL；无 GLFW 环境不纳入默认构建（见 CMake
 * `AURORA_BACKEND_GLFW`，由 `AURORA_BACKEND_GLFW` 开关控制，默认 OFF）。GLFW 初始化失败
 * （无显示/驱动）会抛 `std::runtime_error`，调用方需捕获。
 */
class GlfwSurface : public Surface {
  public:
    /// @brief 后端配置。逻辑尺寸为 aurora 坐标系下的像素（不含 DPI 缩放）。
    struct Config {
        Size size{ .width = 800.0f, .height = 600.0f }; ///< 逻辑尺寸（= GLFW 内容尺寸）
        std::string title{ "Aurora" };
        int gl_major = 3;
        int gl_minor = 3;
        bool resizable = true;
    };

    explicit GlfwSurface(const Config &cfg);
    ~GlfwSurface() override;

    GlfwSurface(const GlfwSurface &) = delete;
    auto operator=(const GlfwSurface &) -> GlfwSurface & = delete;
    GlfwSurface(GlfwSurface &&) = delete;
    auto operator=(GlfwSurface &&) -> GlfwSurface & = delete;

    /// @brief 事件处理器：GLFW 原生事件翻译为 aurora `Event` 后上抛（ARCHITECTURE.md §3.1），由 Application 统一派发。
    auto set_event_handler(const EventHandler &h) -> void override;
    /// @brief 注册窗口可见性状态上报句柄（最小化/被遮挡/前台激活）。
    auto set_window_state_handler(WindowStateHandler h) -> void override;
    /// @brief 注册窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen）。
    auto set_window_mode_handler(WindowModeHandler h) -> void override;

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override;
    [[nodiscard]] auto present() -> Result<bool> override;
    [[nodiscard]] auto size() const -> Size override;
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{ 245, 245, 247, 255 }; }
    [[nodiscard]] auto scale_factor() const -> float override;
    [[nodiscard]] auto should_close() const -> bool override;

    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待事件或超时：`glfwWaitEventsTimeout`。
    /// 无限等待按 1s 分段兜底（防丢唤醒死等）。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒：`glfwPostEmptyEvent` 使阻塞在 wait_events 的主循环立即返回（线程安全）。
    auto request_wake() -> void override;

    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    [[nodiscard]] auto frame_count() const -> int override;
    /// @brief 真实窗口截图（含非客户区）：Windows 下经 GLFW 原生 HWND 复用 PrintWindow 路径；
    /// 其它平台/未开 DEBUG 回落 unsupported。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_pimpl;
};

} // namespace aurora
