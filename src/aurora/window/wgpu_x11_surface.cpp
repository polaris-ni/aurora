// ============================================================
// wgpu_x11_surface.cpp — X11 宿主 + WgpuRhi GPU 栅格上屏后端实现
// 见 include/aurora/window/wgpu_x11_surface.h 的设计说明（宿主组合 / 失败分层）。
// ============================================================

#include "aurora/window/wgpu_x11_surface.h"

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && \
    defined(AURORA_BACKEND_X11)

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aurora/core/log.h"

namespace aurora {

// ---- 帧 sink 适配（口径同 Win32 WgpuSurface::Sink） ----

auto WgpuX11Surface::Sink::begin_frame(int width, int height, float scale) -> bool {
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

// ---- 构造 / 析构 ----

WgpuX11Surface::WgpuX11Surface(int width, int height, const std::string &title, const WindowStyleOptions &style,
                               bool vsync)
    : vsync_(vsync) {
    host_ = std::make_unique<X11Surface>(width, height, title, style);
    if (!host_->is_available()) {
        AURORA_LOG_ERROR("wgpu-x11-surface", "X11Surface host creation failed");
        return;
    }
    rhi::WgpuRhiOptions opts;
    // Xlib surface 双句柄同源内嵌宿主：Display* + XID Window（经 uintptr_t 装 void*）。
    opts.native_display = host_->native_display();
    opts.native_window = host_->native_handle();
    opts.vsync = vsync_;
    auto rhi = std::make_unique<rhi::WgpuRhi>(opts);
    if (rhi->valid()) {
        gpu_ = std::move(rhi);
        sink_ = std::make_unique<Sink>(*gpu_, *this);
    } else {
        // 构造期失败：is_available() false，工厂按 RendererPreference 报错/改选后端；
        // 若被直接注入使用，则本类以 XPutImage 路径纯软件上屏。
        AURORA_LOG_WARN("wgpu-x11-surface", "WgpuRhi init failed; falling back to software XPutImage presentation");
    }
}

// 成员逆序析构：sink_/gpu_（wgpu surface 引用 Display/Window）先于 host_ 销毁，顺序正确。
WgpuX11Surface::~WgpuX11Surface() = default;

// ---- 能力与帧调度挂点 ----

auto WgpuX11Surface::is_available() const -> bool {
    return host_ != nullptr && host_->is_available() && gpu_ != nullptr && gpu_->valid();
}

auto WgpuX11Surface::gpu_backend() -> rhi::RhiFrameSink * { return sink_.get(); }

auto WgpuX11Surface::set_vsync(bool on) -> void {
    // v1 口径同 Win32 版：swapchain 配置后不改，运行期设置下次重配置时生效。
    vsync_ = on;
}

// ---- 帧生命周期 ----

auto WgpuX11Surface::begin_frame(int width, int height) -> Result<bool> {
    // 底色缓冲与软件上屏全在内嵌宿主（其 begin_frame 已含 set_scale + fill 底色）。
    return host_->begin_frame(width, height);
}

auto WgpuX11Surface::present() -> Result<bool> {
    if (gpu_frame_active_) {
        // GPU 帧：WgpuRhi::end_frame 已 wgpuSurfacePresent，无需二次上屏。
        gpu_frame_active_ = false;
        ++frame_;
        return Result<bool>{true};
    }
    auto r = host_->present();  // 软件回退帧：XPutImage 上屏
    if (r) {
        ++frame_;
        ++software_present_;  // GPU 生效期间本分支不应到达（见 software_present_count()）
    }
    return r;
}

// ---- 诊断与截图 ----

auto WgpuX11Surface::data() const -> const std::uint8_t * {
#ifdef AURORA_ENABLE_DEBUG
    // GPU 模式：CPU 侧无帧内容（v1 未接 swapchain 读回）→ nullptr（同 Win32 版口径）；
    // 真实窗口画面用 capture_window（合成器下 XGetImage 可抓 GPU 上屏内容）。
    if (gpu_ != nullptr && !gpu_dead_) {
        return nullptr;
    }
    return host_->data();
#else
    return nullptr;
#endif
}

auto WgpuX11Surface::capture_window(const std::string &path) -> Result<bool> {
    return host_->capture_window(path);
}

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_X11 (Linux)
