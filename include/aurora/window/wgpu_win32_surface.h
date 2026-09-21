#pragma once

// ============================================================
// wgpu_win32_surface.h — Win32 宿主 + WgpuRhi GPU 栅格上屏后端
// ------------------------------------------------------------
// 仅当 AURORA_BACKEND_GPU_WGPU 且 AURORA_BACKEND_WIN32 定义时编译（Windows 宿主；
// X11 宿主见 wgpu_x11_surface.h 的 WgpuX11Surface）。
// 与 D3D11Surface 同族：共用 `Win32Host` 宿主，但本类是 **GPU 栅格路径**——
// `Window::present_root` 经 `gpu_backend()` 把帧级 DisplayList 回放至 `rhi::WgpuRhi`，
// 命令在 GPU 端光栅化并直接 present 到 HWND swapchain，消除每帧全屏像素上传。
// 软件回退：WgpuRhi 不可用（工厂层 is_available 闸）或运行期 `begin_frame` 失败
// （Window 永久回退）时，本类以 GDI `SetDIBitsToDevice` 上屏 CPU Painter 帧缓冲。
// ============================================================

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)

#include <memory>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/rhi/rhi_frame_sink.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "aurora/window/surface.h"
#include "aurora/window/win32_host.h"

namespace aurora {

/// @brief Win32 + wgpu GPU 栅格表面：帧级 DisplayList 经 `rhi::WgpuRhi` 光栅并 swapchain 上屏。
///
/// 帧调度契约（`RhiFrameSink`）：`Window::present_gpu_frame` 以**逻辑 dp** 尺寸调
/// `sink.begin_frame`，故本类内置适配器把逻辑尺寸 × scale 折算为设备像素后转发
/// `WgpuRhi::begin_frame`（契约要求设备像素）。present 由 `WgpuRhi::end_frame` 内
/// `wgpuSurfacePresent` 完成，`Surface::present()` 在 GPU 帧上为空操作。
///
/// 失败分层（对齐 D3D11/Glfw 先例）：
/// - 构造期：adapter/device/surface 任一失败 → `is_available()` false，工厂按
///   `RendererPreference` 报错或回退其他 Surface；
/// - 运行期：swapchain 重建失败/设备丢失 → `begin_frame` false → Window 置永久软件
///   回退，本类 present() 走 GDI blit（Painter 帧缓冲仍由 begin_frame 维护）。
class WgpuWin32Surface final : public Surface {
  public:
    WgpuWin32Surface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                     bool vsync = true);
    ~WgpuWin32Surface() override;

    WgpuWin32Surface(const WgpuWin32Surface &) = delete;
    auto operator=(const WgpuWin32Surface &) -> WgpuWin32Surface & = delete;
    WgpuWin32Surface(WgpuWin32Surface &&) = delete;
    auto operator=(WgpuWin32Surface &&) -> WgpuWin32Surface & = delete;

    /// @brief 宿主窗口与 wgpu 后端均就绪（false 时工厂应报错/改选其他后端）。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief GPU 栅格路径当前是否生效：构造期成功且未发生运行期永久回退
    /// （`sink.begin_frame` 返回 false 后转 false，此后 present 走 GDI）。测试/自检用。
    [[nodiscard]] auto gpu_active() const -> bool { return gpu_ != nullptr && !gpu_dead_; }

    /// @brief 经 GDI 软件路径上屏的帧数——**GPU 生效期间应为 0**（口径同 WgpuWaylandSurface）。
    /// 非零即「app 帧未走 GPU 通道」：软件回退，或系统要求的重绘绕过了 GPU 帧路径（GPU 模式下
    /// Painter 帧缓冲只维护底色，此类 present 上屏即白闪），故本计数是白闪缺陷的观测签名。
    [[nodiscard]] auto software_present_count() const -> int { return software_present_; }

    /// @brief GPU 帧调度挂点：wgpu 后端可用时返回帧 sink 适配器（恒非空于 is_available）。
    [[nodiscard]] auto gpu_backend() -> rhi::RhiFrameSink * override;

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override { return painter_; }
    [[nodiscard]] auto present() -> Result<bool> override;
    [[nodiscard]] auto size() const -> Size override { return win_->size(); }
    [[nodiscard]] auto scale_factor() const -> float override { return win_->scale_factor(); }
    [[nodiscard]] auto should_close() const -> bool override { return win_->should_close(); }
    auto poll_platform_events() -> void override { win_->poll_platform_events(); }
    auto wait_events(double timeout_ms) -> void override { win_->wait_events(timeout_ms); }
    auto request_wake() -> void override { win_->request_wake(); }
    [[nodiscard]] auto pumps_thread_queue() const -> bool override { return true; }
    [[nodiscard]] auto waits_thread_queue() const -> bool override { return true; }
    auto set_event_handler(const EventHandler &h) -> void override { win_->set_event_handler(h); }
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        win_->set_window_state_handler(std::move(h));
    }
    auto set_window_mode_handler(WindowModeHandler h) -> void override { win_->set_window_mode_handler(std::move(h)); }
    auto set_present_request(PresentRequest h) -> void override { win_->set_present_request(std::move(h)); }
    auto set_owner(const Surface *owner) -> void override {
        win_->set_owner(owner != nullptr ? owner->native_handle() : nullptr);
    }
    auto set_enabled(bool on) -> void override { win_->set_enabled(on); }
    auto raise() -> void override { win_->raise(); }
    auto focus_window() -> void override { win_->focus_window(); }
    [[nodiscard]] auto display_id() const -> int override { return win_->display_id(); }
    [[nodiscard]] auto position() const -> Point override { return win_->position(); }
    auto set_position(Point p) -> void override { win_->set_position(p); }
    auto set_size(Size s) -> void override { win_->set_size(s); }
    auto set_scale_change_handler(ScaleChangeHandler h) -> void override {
        win_->set_scale_change_handler(std::move(h));
    }
    auto set_title(const std::string &title) -> void override { win_->set_title(title); }
    auto set_cursor(CursorShape shape) -> void override;
    auto begin_window_move() -> void override;
    auto begin_window_resize(WindowResizeEdge edge) -> void override;

    /// @brief vsync 开启且 GPU 路径可用时，FIFO present 阻塞到 vblank 自带帧节拍。
    [[nodiscard]] auto paces_frames() const -> bool override { return gpu_ != nullptr && vsync_; }
    /// @brief 启用/关闭垂直同步（构造后设置仅影响后续帧；swapchain 重配置时生效）。
    auto set_vsync(bool on) -> void;
    [[nodiscard]] auto vsync() const -> bool { return vsync_; }

    /// @brief begin_frame 铺的浅色底色（与 Win32Surface 同色，脏区裁剪重绘重铺同源）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }

    /// @brief GPU 帧的 CPU 读回 v1 未接（Painter 缓冲在 GPU 模式下不含帧内容）：返回
    /// nullptr 使 `save_snapshot` 明确报 unsupported；真实窗口截图走 `capture_window`。
    /// 软件回退帧返回 Painter 缓冲（DEBUG 下）。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;

    /// @brief 帧缓冲物理像素尺寸（painter 按 DPI 物理分辨率分配，同 Win32Surface 口径）。
    [[nodiscard]] auto framebuffer_size() const -> Size override {
        return Size{.width = static_cast<float>(painter_.width()), .height = static_cast<float>(painter_.height())};
    }
    /// @brief 真实窗口截图（含非客户区）：共享 `detail::capture_window_by_hwnd`
    /// （PrintWindow PW_RENDERFULLCONTENT，可抓 GPU swapchain 内容）。DEBUG 下生效。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;

    /// @brief 已呈现帧数（GPU 帧与 GDI 回退帧均计数）。
    [[nodiscard]] auto frame_count() const -> int override { return frame_; }

    [[nodiscard]] auto hwnd() const -> void * { return win_->hwnd(); }
    [[nodiscard]] auto native_handle() const -> void * override { return win_->hwnd(); }
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override {
        return win_->accessibility_provider();
    }
    auto set_accessibility_root(Widget *root) -> void override { win_->set_accessibility_root(root); }
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override {
        win_->set_composition_caret_provider(std::move(provider));
    }

  private:
    /// @brief 帧 sink 适配器：把 `Window::present_gpu_frame` 传入的逻辑 dp 尺寸折算为
    /// 设备像素（契约要求 `begin_frame` 收设备像素），再转发 `WgpuRhi`；并记录本帧是否
    /// 走了 GPU 路径（present 据此跳过 GDI blit）。
    class Sink final : public rhi::RhiFrameSink {
      public:
        Sink(rhi::WgpuRhi &rhi, WgpuWin32Surface &owner) : rhi_(&rhi), owner_(&owner) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "gpu-wgpu"; }
        [[nodiscard]] auto backend() -> rhi::RhiBackend & override { return *rhi_; }
        [[nodiscard]] auto begin_frame(int width, int height, float scale) -> bool override;
        auto end_frame() -> void override { rhi_->end_frame(); }

      private:
        rhi::WgpuRhi *rhi_;
        WgpuWin32Surface *owner_;
    };

    /// @brief 软件回退上屏：RGBA Painter 缓冲 swizzle 到 BGRA 暂存后 `SetDIBitsToDevice`。
    auto present_gdi() -> void;

    std::unique_ptr<Win32Host> win_;  ///< 共享窗口宿主（同 D3D11Surface 模式）
    Painter painter_;  ///< CPU 帧缓冲：软件回退路径的绘制目标（GPU 模式维护底色缓冲）
    std::unique_ptr<rhi::WgpuRhi> gpu_;  ///< wgpu 后端（nullptr = 初始化失败，纯软件回退）
    std::unique_ptr<Sink> sink_;  ///< 帧 sink 适配器（与 gpu_ 同生命周期）
    std::vector<std::uint32_t> bgra_;  ///< 软件回退上屏的 BGRA swizzle 暂存

    bool vsync_ = true;
    bool gpu_frame_active_ = false;  ///< 本帧 sink.begin_frame 成功（present 时消费）
    bool gpu_dead_ = false;  ///< 运行期 GPU 失效（sink.begin_frame 返回 false，永久软件回退）
    int frame_ = 0;  ///< 已呈现帧计数
    int software_present_ = 0;  ///< GDI 软件路径上屏帧数（见 software_present_count()）
};

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32
