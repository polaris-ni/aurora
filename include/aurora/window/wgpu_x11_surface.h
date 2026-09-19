#pragma once
#include "aurora/core/platform.h"  // 门控依赖 AURORA_PLATFORM_LINUX/ANDROID，须在守卫求值前可见

// ============================================================
// wgpu_x11_surface.h — X11 宿主 + WgpuRhi GPU 栅格上屏后端
// ------------------------------------------------------------
// 仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_X11（Linux）定义时编译。
// 与 Win32 的 WgpuSurface 同族、同帧调度契约：`Window::present_root` 经
// `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，GPU 端光栅化并经
// Xlib surface（`WGPUSurfaceSourceXlibWindow`，Display* 与 XID 同源于
// `X11Surface::native_display()/native_handle()`）present 上屏。
// 宿主复用：组合内嵌 `X11Surface`（窗口创建/事件泵/光标/标题/几何全走它），
// 本类只做「GPU 帧路径 + 软件回退分流」，不复制 Xlib 逻辑，公共头零 Xlib 依赖。
// 软件回退：WgpuRhi 不可用（构造期）或运行期 `begin_frame` 失败（永久回退）时，
// present 委托内嵌 X11Surface 的 XPutImage 路径。
// 与 Win32 版差异（如实申报）：无 DPI per-monitor 变更回调、无 UIA/IMM32 桥
// （X11 侧本就没有对应实现，X11Surface 也未覆写这些扩展点）。
// ============================================================

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_X11)

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/x11_surface.h"

namespace aurora {

/// @brief X11 + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）与 Win32 `WgpuSurface` 完全一致：`Window::present_gpu_frame`
/// 以**逻辑 dp** 尺寸调 `sink.begin_frame`，本类内置适配器按内嵌宿主 scale 折算设备像素。
///
/// 失败分层（对齐 Win32 版）：
/// - 构造期：X 连接/窗口或 adapter/device/surface 任一失败 → `is_available()` false；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 永久回退，
///   本类 present() 委托 `X11Surface::present()`（XPutImage 软件上屏）。
class WgpuX11Surface final : public Surface {
  public:
    WgpuX11Surface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                   bool vsync = true);
    ~WgpuX11Surface() override;

    WgpuX11Surface(const WgpuX11Surface &) = delete;
    auto operator=(const WgpuX11Surface &) -> WgpuX11Surface & = delete;
    WgpuX11Surface(WgpuX11Surface &&) = delete;
    auto operator=(WgpuX11Surface &&) -> WgpuX11Surface & = delete;

    /// @brief 内嵌 X11 宿主与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效（回退观测点，语义同 Win32 WgpuSurface::gpu_active）。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    // ---- 帧生命周期：GPU 帧走 sink，软件帧委托内嵌 X11Surface（XPutImage） ----
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override { return host_->painter(); }
    [[nodiscard]] auto present() -> Result<bool> override;

    // ---- 宿主转发（窗口几何/事件/标题/光标均在内嵌 X11Surface） ----
    [[nodiscard]] auto size() const -> Size override { return host_->size(); }
    [[nodiscard]] auto scale_factor() const -> float override { return host_->scale_factor(); }
    [[nodiscard]] auto should_close() const -> bool override { return host_->should_close(); }
    auto poll_platform_events() -> void override { host_->poll_platform_events(); }
    auto wait_events(double timeout_ms) -> void override { host_->wait_events(timeout_ms); }
    auto request_wake() -> void override { host_->request_wake(); }
    /// @brief X11 泵只抽本连接队列（基类默认 false），显式转发保持与内嵌宿主同口径。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return host_->pumps_thread_queue(); }
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return host_->waits_thread_queue(); }
    auto set_event_handler(const EventHandler &h) -> void override { host_->set_event_handler(h); }
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        host_->set_window_state_handler(std::move(h));
    }
    auto set_window_mode_handler(WindowModeHandler h) -> void override { host_->set_window_mode_handler(std::move(h)); }
    auto set_present_request(PresentRequest h) -> void override { host_->set_present_request(std::move(h)); }
    auto set_title(const std::string &title) -> void override { host_->set_title(title); }
    auto set_cursor(CursorShape shape) -> void override { host_->set_cursor(shape); }
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override {
        host_->set_present_dirty(device_rects);
    }

    /// @brief vsync 开启且 GPU 路径可用时，FIFO present 阻塞到 vblank 自带帧节拍。
    [[nodiscard]] auto paces_frames() const -> bool override { return gpu_ != nullptr && vsync_; }
    /// @brief 启用/关闭垂直同步（口径同 Win32 版：swapchain 重配置时生效）。
    auto set_vsync(bool on) -> void;
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与内嵌 X11Surface 同色）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接：GPU 模式返回 nullptr（save_snapshot 报 unsupported）；
    /// 软件回退帧委托内嵌 X11Surface（DEBUG 下返回 Painter 缓冲）。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 真实窗口截图：委托内嵌 X11Surface 的 XGetImage 路径（合成器环境下含 GPU 上屏内容）。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

    /// @brief 已呈现帧数（GPU 帧与软件回退帧均计数）。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    /// @brief 原生窗口句柄：内嵌 X11Surface 的 XID（Window）。
    [[nodiscard]] auto native_handle() const -> void * override { return host_->native_handle(); }

  private:
    /// @brief 帧 sink 适配器：逻辑 dp × 宿主 scale → 设备像素转发 `WgpuRhi`，
    /// 并记录本帧是否走 GPU 路径（present 据此分流）。同 Win32 版 WgpuSurface::Sink。
    class Sink final : public rhi::RhiFrameSink {
      public:
        Sink(rhi::WgpuRhi &rhi, WgpuX11Surface &owner) : rhi_(&rhi), owner_(&owner) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }
        [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return *rhi_; }
        [[nodiscard]] auto begin_frame(int width, int height, float scale) -> bool override;
        auto end_frame() -> void override { rhi_->end_frame(); }

      private:
        rhi::WgpuRhi *rhi_;
        WgpuX11Surface *owner_;
    };

    std::unique_ptr<X11Surface> host_;    ///< 内嵌 X11 宿主（窗口/事件/软件回退上屏）
    std::unique_ptr<rhi::WgpuRhi> gpu_;   ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;          ///< 帧 sink 适配器（与 gpu_ 同生命周期）

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;          ///< 运行期 GPU 失效（永久软件回退）
    int frame_ = 0;                  ///< 已呈现帧计数
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_X11 (Linux)
