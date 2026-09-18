// ============================================================
// wgpu_surface.cpp — Win32 宿主 + WgpuRhi GPU 栅格上屏后端实现
// 见 include/aurora/window/wgpu_surface.h 的设计说明（失败分层 / 帧 sink 适配）。
// ============================================================

#include "aurora/window/wgpu_surface.h"

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "aurora/core/log.h"
#include "aurora/window/swizzle.h"
#include "aurora/window/win32_capture.h"
#include "aurora/window/win32_cursor.h"

namespace aurora {

// ---- 帧 sink 适配 ----

auto WgpuSurface::Sink::begin_frame(int width, int height, float scale) -> bool {
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

WgpuSurface::WgpuSurface(int width, int height, const std::string &title, const WindowStyleOptions &style, bool vsync)
    : vsync_(vsync) {
    win_ = std::make_unique<Win32Window>(width, height, title, style);
    if (win_->hwnd() == nullptr) {
        AURORA_LOG_ERROR("wgpu-surface", "Win32Window creation failed");
        return;
    }
    rhi::WgpuRhiOptions opts;
    opts.native_window = win_->hwnd();
    opts.vsync = vsync_;
    auto rhi = std::make_unique<rhi::WgpuRhi>(opts);
    if (rhi->valid()) {
        gpu_ = std::move(rhi);
        sink_ = std::make_unique<Sink>(*gpu_, *this);
    } else {
        // 构造期失败：is_available() false，工厂按 RendererPreference 报错/改选后端；
        // 若被直接注入使用，则本类以 GDI 路径纯软件上屏。
        AURORA_LOG_WARN("wgpu-surface", "WgpuRhi init failed; falling back to software GDI presentation");
    }
}

// 成员逆序析构：sink_/gpu_（wgpu surface 引用 HWND）先于 win_ 销毁，顺序正确。
WgpuSurface::~WgpuSurface() = default;

// ---- 能力与帧调度挂点 ----

auto WgpuSurface::is_available() const -> bool {
    return win_ != nullptr && win_->hwnd() != nullptr && gpu_ != nullptr && gpu_->valid();
}

auto WgpuSurface::gpu_backend() -> rhi::RhiFrameSink * { return sink_.get(); }

auto WgpuSurface::set_vsync(bool on) -> void {
    // v1 口径：vsync 以构造期选项为准（swapchain 配置后不改）；运行期设置仅记录，
    // 下次窗口尺寸变化触发 swapchain 重配置时生效。
    vsync_ = on;
}

// ---- 帧生命周期 ----

auto WgpuSurface::begin_frame(int width, int height) -> Result<bool> {
    const float s = scale_factor();
    // Painter 按 scale 把逻辑 dp 映射到物理像素：begin 传逻辑尺寸（与 Win32/D3D11 同源）。
    // GPU 模式下 Window 已先行 p.record(frame_dl)：底色 fill_rect 落入帧 DL（Glfw GPU 模式同构），
    // 软件回退模式下同一批调用直接光栅化进缓冲。
    painter_.set_scale(s);
    painter_.begin(width, height);
    painter_.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                            .size = Size{.width = static_cast<float>(width), .height = static_cast<float>(height)}},
                       clear_color());
    return Result<bool>{true};
}

auto WgpuSurface::present() -> Result<bool> {
    if (gpu_frame_active_) {
        // GPU 帧：WgpuRhi::end_frame 已 wgpuSurfacePresent，无需二次上屏。
        gpu_frame_active_ = false;
        ++frame_;
        return Result<bool>{true};
    }
    present_gdi();
    ++frame_;
    return Result<bool>{true};
}

auto WgpuSurface::present_gdi() -> void {
    const int w = painter_.width();
    const int h = painter_.height();
    if (w <= 0 || h <= 0) {
        return;
    }
    bgra_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    swizzle_rgba_to_bgra(reinterpret_cast<const std::uint32_t *>(painter_.data()), bgra_.data(), bgra_.size());
    HDC hdc = GetDC(static_cast<HWND>(win_->hwnd()));
    if (hdc == nullptr) {
        return;
    }
    // BI_RGB 32bpp = GDI 原生 BGRA 序：免逐像素慢速转换（口径同 Win32Surface）。
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down，与 Painter 一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 0, 0, static_cast<UINT>(w),
                      static_cast<UINT>(h), bgra_.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(static_cast<HWND>(win_->hwnd()), hdc);
}

// ---- 诊断与截图 ----

auto WgpuSurface::data() const -> const std::uint8_t * {
#ifdef AURORA_ENABLE_DEBUG
    // GPU 模式：CPU 侧无帧内容（v1 未接 swapchain 读回）→ nullptr，save_snapshot 明确
    // 报 unsupported，真实窗口画面用 capture_window（PrintWindow 可抓 GPU 上屏）。
    if (gpu_ != nullptr && !gpu_dead_) {
        return nullptr;
    }
    return painter_.data();
#else
    return nullptr;
#endif
}

auto WgpuSurface::capture_window(const std::string &path) -> Result<bool> {
#ifdef AURORA_ENABLE_DEBUG
    // 与 Win32/D3D11 同源：共享 PrintWindow(PW_RENDERFULLCONTENT) 路径，可抓 GPU swapchain 内容。
    return detail::capture_window_by_hwnd(static_cast<HWND>(native_handle()), path);
#else
    (void)path;
    return Result<bool>{
        make_error(ErrorCode::GeneralNotSupported, "capture_window: disabled (AURORA_ENABLE_DEBUG not enabled)")};
#endif
}

// ---- 窗口行为（宿主转发）----

auto WgpuSurface::set_cursor(CursorShape shape) -> void { detail::set_win32_cursor(shape); }

auto WgpuSurface::begin_window_move() -> void {
    PostMessageW(static_cast<HWND>(win_->hwnd()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

auto WgpuSurface::begin_window_resize(WindowResizeEdge edge) -> void {
    // 序对应 WindowResizeEdge 枚举值序（同 Win32Surface）。
    static constexpr std::array<int, 9> HT = {HTNOWHERE, HTTOP,      HTBOTTOM,     HTLEFT,       HTRIGHT,
                                              HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT};
    const auto idx = static_cast<std::size_t>(edge);
    if (idx != 0 && idx < HT.size()) {
        PostMessageW(static_cast<HWND>(win_->hwnd()), WM_NCLBUTTONDOWN, static_cast<WPARAM>(HT.at(idx)), 0);
    }
}

}  // namespace aurora

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32
