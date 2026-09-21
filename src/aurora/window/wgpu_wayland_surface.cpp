// ============================================================
// wgpu_wayland_surface.cpp — Wayland 宿主 + WgpuRhi GPU 栅格上屏后端实现
// 见 include/aurora/window/wgpu_wayland_surface.h 的设计说明（宿主组合 / 失败分层）。
// ============================================================

#include "aurora/window/wgpu_wayland_surface.h"

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_WAYLAND)

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aurora/core/log.h"

namespace aurora {

// ---- 帧 sink 适配（口径同 Win32/X11 版内置 Sink） ----

auto WgpuWaylandSurface::Sink::begin_frame(int width, int height, float scale) -> bool {
    // `Window::present_gpu_frame` 传逻辑 dp 尺寸；`RhiFrameSink` 契约要求设备像素。
    const float s = scale > 0.0F ? scale : 1.0F;
    const int dev_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * s)));
    const int dev_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * s)));
    const bool ok = rhi_->begin_frame(dev_w, dev_h, s);
    owner_->gpu_frame_active_ = ok;
    if (!ok) {
        owner_->gpu_dead_ = true;  // 运行期失效：此后 data() 可读软件回退帧缓冲
    }
    return ok;
}

auto WgpuWaylandSurface::Sink::end_frame() -> void {
    // 自绘 CSD 装饰合成进当帧：swapchain 独占整块 wl_surface，内嵌宿主画进 Painter 的标题栏
    // 不会随帧缓冲上屏，故装饰只能作为命令在 app 帧之上回放开（软件路径画的是同一份内容，
    // 两条路径共用 `csd::paint_title_bar`，见 WaylandSurface::record_client_decoration）。
    // 坐标同为逻辑 dp、同用本帧 scale，与 app 帧 DL 同一变换口径，故热区与像素不会错位。
    if (owner_->host_->record_client_decoration(deco_dl_)) {
        deco_dl_.replay(*rhi_);
        ++owner_->deco_replays_;
    }
    rhi_->end_frame();
}

// ---- 构造 / 析构 ----

WgpuWaylandSurface::WgpuWaylandSurface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                                       bool vsync)
    : vsync_(vsync) {
    host_ = std::make_unique<WaylandSurface>(width, height, title, style);
    if (!host_->is_available()) {
        AURORA_LOG_ERROR("wgpu-wayland-surface", "WaylandSurface host creation failed");
        return;
    }
    rhi::WgpuRhiOptions opts;
    // Wayland surface 双句柄同源内嵌宿主：wl_display* + wl_surface*（均 void* 直传）。
    opts.linux_host = rhi::WgpuRhiOptions::LinuxHost::Wayland;
    opts.native_display = host_->native_display();
    opts.native_window = host_->native_handle();
    opts.vsync = vsync_;
    auto rhi = std::make_unique<rhi::WgpuRhi>(opts);
    if (rhi->valid()) {
        gpu_ = std::move(rhi);
        sink_ = std::make_unique<Sink>(*gpu_, *this);
    } else {
        // 构造期失败：is_available() false，工厂据此返回 Result 错误（错误归属调用方）；
        // 若被直接注入使用，则本类以 wl_shm 路径纯软件上屏。
        AURORA_LOG_WARN("wgpu-wayland-surface", "WgpuRhi init failed; falling back to software wl_shm presentation");
    }
}

// 成员逆序析构：sink_/gpu_（wgpu surface 引用 wl_display/wl_surface）先于 host_ 销毁，
// 顺序正确——WaylandSurface 析构才 disconnect，wgpu 释放 surface 时连接仍有效。
WgpuWaylandSurface::~WgpuWaylandSurface() = default;

// ---- 能力与帧调度挂点 ----

auto WgpuWaylandSurface::is_available() const -> bool {
    return host_ != nullptr && host_->is_available() && gpu_ != nullptr && gpu_->valid();
}

auto WgpuWaylandSurface::gpu_backend() -> rhi::RhiFrameSink * { return sink_.get(); }

auto WgpuWaylandSurface::set_vsync(bool on) -> void {
    // v1 口径同 Win32/X11 版：swapchain 配置后不改，运行期设置下次重配置时生效。
    vsync_ = on;
}

// ---- 帧生命周期 ----

auto WgpuWaylandSurface::begin_frame(int width, int height) -> Result<bool> {
    // 底色缓冲与软件上屏全在内嵌宿主（其 begin_frame 已含底色 fill 与尺寸对齐）。
    return host_->begin_frame(width, height);
}

auto WgpuWaylandSurface::present() -> Result<bool> {
    if (gpu_frame_active_) {
        // GPU 帧：WgpuRhi::end_frame 已 wgpuSurfacePresent（commit wl_surface），无需二次上屏。
        gpu_frame_active_ = false;
        ++frame_;
        return Result<bool>{true};
    }
    auto r = host_->present();  // 软件回退帧：wl_shm attach+commit 上屏
    if (r) {
        ++frame_;
        ++software_present_;  // GPU 生效期间本分支不应到达（见 software_present_count()）
    }
    return r;
}

// ---- 诊断与截图 ----

auto WgpuWaylandSurface::data() const -> const std::uint8_t * {
#ifdef AURORA_ENABLE_DEBUG
    // GPU 模式：CPU 侧无帧内容（v1 未接 swapchain 读回）→ nullptr（同 Win32/X11 版口径）；
    // Wayland 亦无 capture_window（协议无抓屏原语），基类默认报 disabled。
    if (gpu_ != nullptr && !gpu_dead_) {
        return nullptr;
    }
    return host_->data();
#else
    return nullptr;
#endif
}

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WAYLAND (Linux)
