#pragma once
#include "aurora/core/platform.h"  // 门控依赖 AURORA_PLATFORM_LINUX/ANDROID，须在守卫求值前可见

// ============================================================
// wgpu_wayland_surface.h — Wayland 宿主 + WgpuRhi GPU 栅格上屏后端
// ------------------------------------------------------------
// 仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_WAYLAND（Linux）定义时编译。
// 与 Win32 的 WgpuSurface、Linux/X11 的 WgpuX11Surface 同族、同帧调度契约：
// `Window::present_root` 经 `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，
// GPU 端光栅化并经 Wayland surface（`WGPUSurfaceSourceWaylandSurface`，wl_display*
// 与 wl_surface* 同源于 `WaylandSurface::native_display()/native_handle()`）present 上屏。
// 宿主复用：组合内嵌 `WaylandSurface`（窗口壳/事件泵/xkb 输入/CSD/几何全走它），
// 本类只做「GPU 帧路径 + 软件回退分流」，不复制 wayland-client 逻辑，公共头零
// wayland 依赖。
// 软件回退：WgpuRhi 不可用（构造期）或运行期 `begin_frame` 失败（永久回退）时，
// present 委托内嵌 WaylandSurface 的 wl_shm 路径。
// 与 X11 版差异（如实申报）：
// - 无 `capture_window`（Wayland 协议无抓屏原语，内嵌宿主同样未覆写，基类默认
//   报 disabled 即最终行为）；
// - 自绘 CSD 装饰经命令通道合成进 GPU 帧：swapchain 独占整块 wl_surface，内嵌宿主画进
//   Painter 的标题栏不会随帧缓冲上屏，故每帧在 `Sink::end_frame` 里把装饰录制成
//   DisplayList 追加回放在 app 帧之后（绘制实现与软件路径同源 `csd::paint_title_bar`，
//   见 `WaylandSurface::record_client_decoration`）。合成器提供 xdg-decoration SSD 时
//   内嵌宿主不绘装饰，录制返回 false → 零额外开销。
// ============================================================

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_WAYLAND)

#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"  // Sink 的装饰录制缓冲成员需完整类型
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/wayland_surface.h"

namespace aurora {

/// @brief Wayland + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）与 Win32 `WgpuSurface`、X11 `WgpuX11Surface` 完全一致：
/// `Window::present_gpu_frame` 以**逻辑 dp** 尺寸调 `sink.begin_frame`，本类内置适配器
/// 按内嵌宿主 scale 折算设备像素。
///
/// 失败分层（对齐兄弟宿主）：
/// - 构造期：Wayland 连接/窗口壳或 adapter/device/surface 任一失败 → `is_available()` false；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 永久回退，
///   本类 present() 委托 `WaylandSurface::present()`（wl_shm 软件上屏）。
class WgpuWaylandSurface final : public Surface {
  public:
    WgpuWaylandSurface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                       bool vsync = true);
    ~WgpuWaylandSurface() override;

    WgpuWaylandSurface(const WgpuWaylandSurface &) = delete;
    auto operator=(const WgpuWaylandSurface &) -> WgpuWaylandSurface & = delete;
    WgpuWaylandSurface(WgpuWaylandSurface &&) = delete;
    auto operator=(WgpuWaylandSurface &&) -> WgpuWaylandSurface & = delete;

    /// @brief 内嵌 Wayland 宿主与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效（回退观测点，语义同 Win32 WgpuSurface::gpu_active）。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief 经软件路径（内嵌宿主 wl_shm）上屏的帧数——**GPU 生效期间应为 0**。
    /// 非零即「app 帧未走 GPU 通道」：软件回退，或系统要求的重绘绕过了 GPU 帧路径
    /// （后者是白闪缺陷的签名：`Window` 直接调 `present()`，而 GPU 模式下 Painter 帧缓冲
    /// 从不清绘，上屏只剩底色）。真机探针据此断言「无白闪帧」。
    [[nodiscard]] auto software_present_count() const -> int { return software_present_; }

    /// @brief 把自绘 CSD 装饰回放进 GPU 帧的帧数（观测点）。合成器提供 SSD 时内嵌宿主不绘
    /// 装饰，本计数恒 0；CSD 兜底合成器（GNOME 等）下应逐帧递增 = GPU 呈现帧数。
    [[nodiscard]] auto decoration_replay_count() const -> int { return deco_replays_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    // ---- 帧生命周期：GPU 帧走 sink，软件帧委托内嵌 WaylandSurface（wl_shm） ----
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override { return host_->painter(); }
    [[nodiscard]] auto present() -> Result<bool> override;

    // ---- 宿主转发（窗口几何/事件/标题/光标/CSD 均在内嵌 WaylandSurface） ----
    [[nodiscard]] auto size() const -> Size override { return host_->size(); }
    [[nodiscard]] auto scale_factor() const -> float override { return host_->scale_factor(); }
    [[nodiscard]] auto should_close() const -> bool override { return host_->should_close(); }
    auto poll_platform_events() -> void override { host_->poll_platform_events(); }
    auto wait_events(double timeout_ms) -> void override { host_->wait_events(timeout_ms); }
    auto request_wake() -> void override { host_->request_wake(); }
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
    /// @brief 启用/关闭垂直同步（口径同 Win32/X11 版：swapchain 重配置时生效）。
    auto set_vsync(bool on) -> void;
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与内嵌 WaylandSurface 同色）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接：GPU 模式返回 nullptr（save_snapshot 报 unsupported）；
    /// 软件回退帧委托内嵌 WaylandSurface（DEBUG 下返回 Painter 缓冲）。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 已呈现帧数（GPU 帧与软件回退帧均计数）。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    /// @brief 原生窗口句柄：内嵌 WaylandSurface 的 `wl_surface*`。
    [[nodiscard]] auto native_handle() const -> void * override { return host_->native_handle(); }

    // ---- CSD 与窗口动作：全部落在内嵌宿主（语义同 WaylandSurface 本体） ----
    auto set_title_bar_style(const TitleBarStyle &style) -> void override { host_->set_title_bar_style(style); }
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void override { host_->set_title_bar_icon(icon); }
    auto begin_window_move() -> void override { host_->begin_window_move(); }
    auto begin_window_resize(WindowResizeEdge edge) -> void override { host_->begin_window_resize(edge); }
    [[nodiscard]] auto content_inset() const -> EdgeInsets override { return host_->content_inset(); }
    auto close() -> void override { host_->close(); }
    auto minimize() -> void override { host_->minimize(); }
    auto toggle_maximize() -> void override { host_->toggle_maximize(); }
    auto set_fullscreen(bool on) -> void override { host_->set_fullscreen(on); }

  private:
    /// @brief 帧 sink 适配器：逻辑 dp × 宿主 scale → 设备像素转发 `WgpuRhi`，
    /// 并记录本帧是否走 GPU 路径（present 据此分流）。同 Win32/X11 版内置 Sink。
    class Sink final : public rhi::RhiFrameSink {
      public:
        Sink(rhi::WgpuRhi &rhi, WgpuWaylandSurface &owner) : rhi_(&rhi), owner_(&owner) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }
        [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return *rhi_; }
        [[nodiscard]] auto begin_frame(int width, int height, float scale) -> bool override;
        /// @brief 收口本帧：先把自绘 CSD 装饰回放在 app 帧之上（swapchain 独占 wl_surface，
        /// 装饰只能走命令通道），再交 `WgpuRhi::end_frame` 提交 + present。见 `.cpp`。
        auto end_frame() -> void override;

      private:
        rhi::WgpuRhi *rhi_;
        WgpuWaylandSurface *owner_;
        DisplayList deco_dl_;  ///< 装饰录制缓冲（逐帧复用，避免每帧分配）
    };

    std::unique_ptr<WaylandSurface> host_;  ///< 内嵌 Wayland 宿主（窗口壳/事件/软件回退上屏）
    std::unique_ptr<rhi::WgpuRhi> gpu_;     ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;            ///< 帧 sink 适配器（与 gpu_ 同生命周期）

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;          ///< 运行期 GPU 失效（永久软件回退）
    int frame_ = 0;                  ///< 已呈现帧计数
    int software_present_ = 0;       ///< 软件路径上屏帧数（见 software_present_count()）
    int deco_replays_ = 0;           ///< 装饰回放进 GPU 帧的帧数（见 decoration_replay_count()）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WAYLAND (Linux)
