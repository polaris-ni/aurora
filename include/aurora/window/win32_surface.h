#pragma once

// Win32/GDI 原生窗口后端：零三方依赖（仅 user32/gdi32），仅 `AURORA_PLATFORM_WINDOWS` 下编译。
// 窗口宿主（创建/消息泵/事件翻译/DPI/同步重渲染）抽取到共享 `win32_window.h`，
// 本类仅负责 GDI 上屏（SetDIBitsToDevice 把软件 Painter 帧缓冲拷到窗口）。
// 重构后须保证行为与抽取前逐位等价：WM_SIZE/WM_PAINT 触发同步重渲染、白闪修复刷不变。
#ifdef AURORA_BACKEND_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN // NOLINT(readability-identifier-naming)
#endif
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>

#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/environment/media_query.h"
#include "aurora/render/painter.h"
#include "aurora/window/surface.h"
#include "aurora/window/win32_window.h"

namespace aurora {

/// @brief Win32/GDI 后端：软件 Painter 帧缓冲经常驻 BGRA DIB section + `BitBlt` 上屏。
///
/// 窗口宿主（`Win32Window`）负责创建/消息/事件/DPI/同步重渲染；本类只做 GDI blit，
/// 与 `D3D11Surface` 共用宿主但后端不同。上屏路径：RGBA 帧缓冲 CPU swizzle 到
/// BGRA（GDI 原生序）DIB section 后 `BitBlt`——非原生 RGBA 掩码会迫使 GDI 逐像素
/// 慢速转换（5760×3132px 实测 87ms），swizzle+BitBlt 仅 ~9ms（bench_win32_present ④）。
class Win32Surface final : public Surface {
  public:
    Win32Surface(int w, int h, const std::string &title) : Win32Surface(w, h, title, WindowStyleOptions{}) {}
    Win32Surface(int w, int h, const std::string &title, const WindowStyleOptions &style)
        : m_win(std::make_unique<Win32Window>(w, h, title, style)) {}
    ~Win32Surface() override { release_dib(); }

    Win32Surface(const Win32Surface &) = delete;
    auto operator=(const Win32Surface &) -> Win32Surface & = delete;
    Win32Surface(Win32Surface &&) = delete;
    auto operator=(Win32Surface &&) -> Win32Surface & = delete;

    /// @brief 事件处理器：Win32 消息翻译为 aurora `Event` 后上抛，由 Application 统一派发。
    auto set_event_handler(const EventHandler &h) -> void override { m_win->set_event_handler(h); }
    /// @brief 注册窗口可见性状态上报句柄（最小化/被遮挡/前台激活）。
    auto set_window_state_handler(WindowStateHandler h) -> void override {
        m_win->set_window_state_handler(std::move(h));
    }
    /// @brief 注册窗口几何态上报句柄（Normal/Maximized/Minimized/FullScreen）。
    auto set_window_mode_handler(WindowModeHandler h) -> void override { m_win->set_window_mode_handler(std::move(h)); }
    /// @brief 同步重渲染请求（由 Window 注入 present_root）：WM_SIZE/WM_PAINT 触发。
    auto set_present_request(PresentRequest h) -> void override { m_win->set_present_request(std::move(h)); }
    /// @brief 运行时更新窗口标题（转发给共享宿主）。
    auto set_title(const std::string &title) -> void override { m_win->set_title(title); }

    /// @brief 控件发起窗口拖拽移动（Win32：伪装 NC 拖拽 HTCAPTION）。
    auto begin_window_move() -> void override {
        PostMessageW(static_cast<HWND>(m_win->hwnd()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    /// @brief 控件发起窗口边缘缩放（Win32：伪装 NC 拖拽对应 HT 边缘码）。
    auto begin_window_resize(WindowResizeEdge edge) -> void override {
        // 序对应 WindowResizeEdge 枚举值序：None/Top/Bottom/Left/Right/TopLeft/TopRight/BottomLeft/BottomRight。
        static constexpr std::array<int, 9> ht = { HTNOWHERE, HTTOP,      HTBOTTOM,     HTLEFT,       HTRIGHT,
                                                   HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT };
        const auto idx = static_cast<std::size_t>(edge);
        if (idx != 0 && idx < ht.size()) {
            PostMessageW(static_cast<HWND>(m_win->hwnd()), WM_NCLBUTTONDOWN, static_cast<WPARAM>(ht.at(idx)), 0);
        }
    }

    /// @brief 增量上屏脏区（设备坐标）：非空时 present() 仅 blit 脏矩形并界，而非整窗——
    /// 大窗口下整窗 SetDIBitsToDevice 是拖选帧的绝对大头（5760×3132px 实测 ~130ms，
    /// 占帧成本 93%）。脏区一次性消费（present 后清空），未设置则全量 blit（首帧/尺寸变化/
    /// WM_PAINT 兜底路径行为不变）。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override { m_present_dirty = device_rects; }

    /// @brief 类背景擦除刷（测试/自检用）：非空表示已消除最大化黑屏。
    [[nodiscard]] static auto background_brush() -> void * { return Win32Window::background_brush(); }
    /// @brief 原生窗口句柄（测试/自检用）：可向该句柄发送 WM_PAINT/WM_SIZE 验证黑屏修复。
    [[nodiscard]] auto hwnd() const -> void * { return m_win->hwnd(); }
    [[nodiscard]] auto native_handle() const -> void * override { return m_win->hwnd(); }
    /// @brief 已呈现次数（测试/自检用）：验证 WM_SIZE/WM_PAINT 触发了同步重渲染。
    [[nodiscard]] auto present_count() const -> int { return m_win->present_count(); }

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override { return m_painter; }
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前帧像素（设备像素缓冲，RGBA）：供 `save_snapshot` 抓帧。
    /// DEBUG 下覆写返回 Painter 缓冲；Release（未开 `AURORA_ENABLE_DEBUG`）回落基类默认值 nullptr，
    /// 使 `save_snapshot` 在 Release 返回 disabled 错误（零截图代码）。
    [[nodiscard]] auto data() const -> const std::uint8_t * override {
#ifdef AURORA_ENABLE_DEBUG
        return m_painter.data();
#else
        return nullptr;
#endif
    }
    /// @brief 帧缓冲物理像素尺寸：Win32 painter 按 DPI 物理分辨率分配，故返回 painter 缓冲像素尺寸，
    /// 而非逻辑 `size()`（缩放比≠1 时二者不同，避免 `save_snapshot` 写出 PNG 宽高与像素数据错位）。
    [[nodiscard]] auto framebuffer_size() const -> Size override {
        return Size{ .width = static_cast<float>(m_painter.width()), .height = static_cast<float>(m_painter.height()) };
    }
    /// @brief 真实窗口截图（含非客户区/标题栏/边框）：经 `PrintWindow` 抓取真实屏幕画面为 PNG。
    /// 覆写基类默认（unsupported）；DEBUG 下调用共享 `detail::capture_window_by_hwnd`，
    /// Release（未开 `AURORA_ENABLE_DEBUG`）回落 unsupported 错误（零截图代码）。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;
    /// @brief 已呈现帧数：复用宿主 `present_count()`，供 `surface_state` 暴露 present 计数。
    [[nodiscard]] auto frame_count() const -> int override { return present_count(); }
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{ 245, 245, 247, 255 }; }
    [[nodiscard]] auto size() const -> Size override { return m_win->size(); }
    [[nodiscard]] auto scale_factor() const -> float override { return m_win->scale_factor(); }
    [[nodiscard]] auto should_close() const -> bool override { return m_win->should_close(); }
    auto poll_platform_events() -> void override { m_win->poll_platform_events(); }
    /// @brief 阻塞等待消息或超时（转发共享宿主）。
    auto wait_events(double timeout_ms) -> void override { m_win->wait_events(timeout_ms); }
    /// @brief 跨线程唤醒主循环（转发共享宿主；PostMessage 线程安全）。
    auto request_wake() -> void override { m_win->request_wake(); }

  private:
    /// @brief 释放常驻 DIB section 与内存 DC（析构/尺寸变化重建时）。
    auto release_dib() -> void;
    /// @brief 确保常驻 BGRA DIB section 与帧缓冲同尺寸（不同则重建）；失败返回 false。
    [[nodiscard]] auto ensure_dib(int w, int h) -> bool;
    /// @brief 全量上屏：整幅 swizzle + 整窗 BitBlt。
    auto present_full(HDC hdc, int w, int h) -> void;
    /// @brief 增量上屏：逐脏矩形 swizzle + BitBlt。
    auto present_dirty(HDC hdc, int w, int h) -> void;

    std::unique_ptr<Win32Window> m_win;
    Painter m_painter;
    std::vector<Rect> m_present_dirty; ///< 本帧增量上屏脏区（设备坐标；空=全量 blit）。
    // 常驻上屏资源：BGRA（GDI 原生序）DIB section，present 时 swizzle+BitBlt。
    HDC m_mem_dc = nullptr;              ///< 内存 DC（DIB 选入其中，BitBlt 源）。
    HBITMAP m_dib = nullptr;             ///< 常驻 BGRA DIB section。
    HGDIOBJ m_dib_old = nullptr;         ///< 选入前的旧位图（释放时换回）。
    std::uint32_t *m_dib_bits = nullptr; ///< DIB 像素内存（BGRA，top-down，行跨 = 宽×4）。
    int m_dib_w = 0;                     ///< DIB 当前宽（物理像素）。
    int m_dib_h = 0;                     ///< DIB 当前高（物理像素）。
};

/// @brief 从 Win32 Surface 构造 `MediaQuery`（含系统 DPI/屏幕/减弱动效）。
/// 内联定义：由 `media_query.cpp` 在 `AURORA_BACKEND_WIN32` 下调用。
inline auto win32_media_query(const Surface &s) -> MediaQuery {
    MediaQuery mq;
    const float scale = s.scale_factor();
    mq.scale_factor = scale;
    mq.size = s.size(); // 逻辑尺寸（设备无关像素）
    const int phys_w = GetSystemMetrics(SM_CXSCREEN);
    const int phys_h = GetSystemMetrics(SM_CYSCREEN);
    mq.screen_size = Size{
        .width = scale > 0.0f ? static_cast<float>(phys_w) / scale : static_cast<float>(phys_w),
        .height = scale > 0.0f ? static_cast<float>(phys_h) / scale : static_cast<float>(phys_h),
    };
    mq.orientation =
        (mq.screen_size.width >= mq.screen_size.height) ? ScreenOrientation::Landscape : ScreenOrientation::Portrait;
    mq.platform = PlatformKind::Windows;
    mq.device = DeviceKind::Desktop;
    BOOL animations_enabled = FALSE;
    if (SystemParametersInfoA(SPI_GETCLIENTAREAANIMATION, 0, &animations_enabled, 0) != 0) {
        mq.prefer_reduced_motion = (animations_enabled == FALSE);
    }
    return mq;
}

} // namespace aurora

#endif // AURORA_BACKEND_WIN32
