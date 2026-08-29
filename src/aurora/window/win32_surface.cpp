#include "aurora/window/win32_surface.h"

#ifdef AURORA_BACKEND_WIN32

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aurora/window/swizzle.h"       // swizzle_rgba_to_bgra 共享实现
#include "aurora/window/win32_capture.h" // detail::capture_window_by_hwnd（真实窗口截图）

namespace aurora {

// 注意：begin_frame/present 仅做 GDI 上屏；窗口创建/消息泵/事件翻译/DPI/同步重渲染
// 全部由共享 Win32Window 宿主负责（与本类共用宿主，行为与原 Win32Surface 逐位等价）。

auto Win32Surface::begin_frame(int width, int height) -> Result<bool> {
    // 新帧从零绘制：上一帧残留的增量脏区对本帧无效（低阶调用方 begin+present 手动拼装时
    // 若沿用旧脏区会漏上屏全量重绘的像素），清空后本帧默认全量 blit；
    // present_root 路径每帧都会在 present 前重新 set_present_dirty，不受影响。
    m_present_dirty.clear();
    const float scale = m_win->scale_factor(); // 当前窗口所在显示器 DPI（支持跨屏移动）
    m_painter.set_scale(scale);

    // 始终以窗口真实客户区（物理像素）为准来分配缓冲与设定逻辑尺寸：
    // WM_SIZE 之后调用方的 size 可能滞后一帧（见 Window::begin_frame 同步时机），
    // 直接查询真实客户区可保证缓冲与屏幕 1:1 吻合，避免最大化/还原后出现黑色区域。
    int phys_w = width > 0 ? static_cast<int>(std::lround(static_cast<float>(width) * scale)) : 0;
    int phys_h = height > 0 ? static_cast<int>(std::lround(static_cast<float>(height) * scale)) : 0;
    auto *const hwnd = static_cast<HWND>(m_win->hwnd());
    if (hwnd != nullptr) {
        RECT cr{};
        if (GetClientRect(hwnd, &cr) != 0) {
            const int cw = cr.right - cr.left;
            const int ch = cr.bottom - cr.top;
            if (cw > 0) {
                phys_w = cw;
            }
            if (ch > 0) {
                phys_h = ch;
            }
        }
    }
    if (phys_w <= 0) {
        phys_w = 1;
    }
    if (phys_h <= 0) {
        phys_h = 1;
    }
    // 软件帧缓冲按物理分辨率分配：几何绘制把 dp 坐标 × scale，1:1 贴窗口避免发虚。
    if (phys_w != m_painter.width() || phys_h != m_painter.height()) {
        const int w = static_cast<int>(std::lround(static_cast<float>(phys_w) / scale));
        const int h = static_cast<int>(std::lround(static_cast<float>(phys_h) / scale));
        m_painter.begin(w, h);
    }
    // 浅色背景：默认文字为黑色，需浅色底才可见（详见 glfw_surface.h 注释）。
    m_painter.fill_rect(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                              .size = Size{ .width = static_cast<float>(m_painter.width()),
                                            .height = static_cast<float>(m_painter.height()) } },
                        Color{ 245, 245, 247, 255 });
    return Result<bool>{ true };
}

auto Win32Surface::release_dib() -> void {
    if (m_mem_dc != nullptr) {
        if (m_dib_old != nullptr) {
            SelectObject(m_mem_dc, m_dib_old);
            m_dib_old = nullptr;
        }
        DeleteDC(m_mem_dc);
        m_mem_dc = nullptr;
    }
    if (m_dib != nullptr) {
        DeleteObject(m_dib);
        m_dib = nullptr;
    }
    m_dib_bits = nullptr;
    m_dib_w = 0;
    m_dib_h = 0;
}

auto Win32Surface::ensure_dib(int w, int h) -> bool {
    if ((m_dib != nullptr) && m_dib_w == w && m_dib_h == h) {
        return true;
    }
    release_dib();
    // BI_RGB 32bpp = GDI 原生 BGRA 序：BitBlt 到窗口 DC 无需逐像素格式转换（快路径）。
    // 若用 RGBA 掩码（BI_BITFIELDS）则 GDI 在 blit 时逐像素慢速转换，与旧
    // SetDIBitsToDevice 一样慢（bench ④ 变体 B 实测 87ms vs 变体 A 5ms）。
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down，与 Painter 一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    m_mem_dc = CreateCompatibleDC(nullptr);
    m_dib = CreateDIBSection(m_mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if ((m_mem_dc == nullptr) || (m_dib == nullptr) || (bits == nullptr)) {
        release_dib();
        return false;
    }
    m_dib_old = SelectObject(m_mem_dc, m_dib);
    m_dib_bits = static_cast<std::uint32_t *>(bits);
    m_dib_w = w;
    m_dib_h = h;
    return true;
}

auto Win32Surface::present_full(HDC hdc, int w, int h) -> void {
    // 全量：整幅 swizzle + 整窗 BitBlt（首帧/尺寸变化/布局帧/低阶调用方）。
    // 旧 SetDIBitsToDevice(RGBA 掩码) 全量 87ms → swizzle+BitBlt ~9ms（最大化卡顿主因）。
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    const auto *src = reinterpret_cast<const std::uint32_t *>(m_painter.data());
    swizzle_rgba_to_bgra(src, m_dib_bits, static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    BitBlt(hdc, 0, 0, w, h, m_mem_dc, 0, 0, SRCCOPY);
}

auto Win32Surface::present_dirty(HDC hdc, int w, int h) -> void {
    // 增量：逐脏矩形仅 swizzle + BitBlt 变化区（拖选帧）。脏矩形向外取整，
    // 覆盖裁剪绘制触及的全部像素（变化像素 ⊆ 同一组矩形构成的裁剪区）。
    // NOLINTNEXTLINE(*-pro-type-reinterpret-cast)
    const auto *src = reinterpret_cast<const std::uint32_t *>(m_painter.data());
    for (const Rect &r : m_present_dirty) {
        const int x0 = std::max(0, static_cast<int>(std::floor(r.origin.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(r.origin.y)));
        const int x1 = std::min(w, static_cast<int>(std::ceil(r.right())));
        const int y1 = std::min(h, static_cast<int>(std::ceil(r.bottom())));
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        for (int y = y0; y < y1; ++y) {
            const std::size_t off =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x0);
            // NOLINTNEXTLINE(*-pro-bounds-pointer-arithmetic)
            swizzle_rgba_to_bgra(src + off, m_dib_bits + off, static_cast<std::size_t>(x1 - x0));
        }
        BitBlt(hdc, x0, y0, x1 - x0, y1 - y0, m_mem_dc, x0, y0, SRCCOPY);
    }
}

auto Win32Surface::present() -> Result<bool> {
    auto *const hwnd = static_cast<HWND>(m_win->hwnd());
    if ((hwnd != nullptr) && (m_painter.data() != nullptr)) {
        const int w = m_painter.width();
        const int h = m_painter.height();
        if (const HDC hdc = GetDC(hwnd); hdc != nullptr) {
            if (ensure_dib(w, h)) {
                if (m_present_dirty.empty()) {
                    present_full(hdc, w, h);
                } else {
                    present_dirty(hdc, w, h);
                }
            }
            ReleaseDC(hwnd, hdc);
        }
    }
    // 脏区一次性消费：下一帧未重新设置则回到全量 blit（行为安全兜底）。
    m_present_dirty.clear();
    return Result<bool>{ true };
}

auto Win32Surface::capture_window(const std::string &path) -> Result<bool> {
#ifdef AURORA_ENABLE_DEBUG
    return detail::capture_window_by_hwnd(static_cast<HWND>(native_handle()), path);
#else
    (void)path;
    return Result<bool>{ make_error(ErrorCode::GeneralNotSupported,
                                    "capture_window: disabled (AURORA_ENABLE_DEBUG not enabled)") };
#endif
}

} // namespace aurora

#endif // AURORA_BACKEND_WIN32
