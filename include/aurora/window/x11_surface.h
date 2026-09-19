#pragma once
#include "aurora/core/platform.h"

// X11/Xlib Surface（ARCHITECTURE.md §8.4）：Linux 桌面原生窗口后端，零三方依赖（仅 libX11）。
// 仅在 defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && AURORA_BACKEND_X11 时提供；
// Wayland 会话下经 XWayland 无缝显示（DISPLAY 由 compositor 提供）。
// Xlib 依赖：Debian/Ubuntu `apt install libx11-dev`；Fedora `dnf install libX11-devel`。
//
// 设计要点：
// - pimpl 隔离：公共头不含 <X11/Xlib.h>，避免 X11 宏（None/Bool/Status…）污染消费者；
//   全部平台逻辑在 src/aurora/window/x11_surface.cpp。
// - 上屏路径：软件 Painter RGBA 帧缓冲 → 按 Visual 掩码 CPU swizzle 到 X 原生像素序
//   （常见 BGRX）→ XPutImage 全量 blit（对齐 Win32Surface 的 swizzle+BitBlt 策略）。
// - 事件翻译：ButtonPress/MotionNotify → MouseEvent；Button4/5 → ScrollEvent；
//   KeyPress/Release → KeyEvent（keysym → KeyCode）；Xutf8LookupString（XIM）→ TextInputEvent；
//   ClientMessage(WM_DELETE_WINDOW) → should_close；FocusIn/Out + Map/Unmap → WindowState。
// - 帧循环：wait_events 经 poll(2) 阻塞在 X 连接 fd + 自唤醒管道；request_wake 线程安全。
// - 构造不抛异常：连接失败（无 DISPLAY/纯 TTY）时 is_available() 为 false，
//   工厂据此返回 Result 错误（错误归属调用方，AI 可枚举）。

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

#include <memory>
#include <string>

#include "aurora/window/surface.h"

namespace aurora {

/// @brief X11/Xlib 后端：软件 Painter 帧缓冲经 XPutImage 上屏，Xlib 事件翻译为 aurora `Event`。
///
/// 一帧生命周期与其他后端一致：`begin_frame` → `painter()` 绘制 → `present`。
/// 窗口样式（置顶/无边框/尺寸限制）经 EWMH/_MOTIF_WM_HINTS/XSizeHints 映射。
/// @note Thread: main-thread only（request_wake 除外，线程安全）
class X11Surface final : public Surface {
  public:
    X11Surface(int w, int h, const std::string &title) : X11Surface(w, h, title, WindowStyleOptions{}) {}
    X11Surface(int w, int h, const std::string &title, const WindowStyleOptions &style);
    ~X11Surface() override;

    X11Surface(const X11Surface &) = delete;
    X11Surface &operator=(const X11Surface &) = delete;

    /// @brief X 连接与窗口是否创建成功（无 DISPLAY/纯 TTY 环境为 false）。
    /// 工厂 `create_window(X11Options)` 据此返回 `Result` 错误而非崩溃。
    [[nodiscard]] auto is_available() const -> bool;

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override;
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前帧像素（设备像素缓冲，RGBA）：DEBUG 下覆写返回 Painter 缓冲；
    /// Release（未开 `AURORA_ENABLE_DEBUG`）回落基类默认值 nullptr，使 `save_snapshot` 返回 disabled。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    /// @brief 真实窗口截图：`XGetImage` 抓窗口 → 按 Visual 掩码提取 RGBA → `write_png`。
    /// 覆写基类默认（unsupported）；Release（未开 `AURORA_ENABLE_DEBUG`）回落 unsupported 错误。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;
    [[nodiscard]] auto size() const -> Size override;
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }
    /// @brief 像素密度：解析 X 资源 `Xft.dpi`（dpi/96），无声明时 1.0。
    [[nodiscard]] auto scale_factor() const -> float override;
    [[nodiscard]] auto should_close() const -> bool override;
    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待 X 事件/唤醒/超时：poll(2) 于 X 连接 fd + 自唤醒管道。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒主循环（线程安全）：向自唤醒管道写 1 字节打断 wait_events。
    auto request_wake() -> void override;

    /// @brief 增量上屏脏区（设备坐标）：非空时 present() 仅 swizzle+XPutImage 脏矩形，
    /// 而非整窗（对齐 Win32Surface 的增量 blit 策略）；脏区一次性消费。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override;

    /// @brief 事件处理器：Xlib 事件翻译为 aurora `Event` 后上抛，由 Application 统一派发。
    auto set_event_handler(const EventHandler &h) -> void override;
    /// @brief 运行时更新窗口标题（XStoreName + _NET_WM_NAME，UTF-8）。
    auto set_title(const std::string &title) -> void override;

    /// @brief 运行时更新悬停光标形状：`XCreateFontCursor` + `XDefineCursor` + `XFlush`。
    /// 句柄按 `CursorShape` 取值序缓存在 Impl（`XCreateFontCursor` 每次调用都产生新资源，
    /// 反复悬停切换必泄漏），析构统一 `XFreeCursor`。
    /// 后端映射：Arrow→`XC_left_ptr`、IBeam→`XC_xterm`、PointingHand→`XC_hand2`、ResizeNS→`XC_sb_v_double_arrow`、
    /// ResizeEW→`XC_sb_h_double_arrow`、ResizeNWSE→`XC_top_left_corner`、ResizeNESW→`XC_top_right_corner`、
    /// Move→`XC_fleur`、Crosshair→`XC_crosshair`、NotAllowed→`XC_X_cursor`、Wait→`XC_watch`。
    /// @note 真机已验证（2026-09-13）：以 `AURORA_BACKEND_X11=ON` 编译通过；并在真实 X server
    /// 上运行时读回（XFIXES `XFixesGetCursorImage`）确认 11 个形状逐个改变了屏幕显示光标且两两互异，
    /// 名称与上表逐项吻合（left_ptr / xterm / hand2 / sb_v_double_arrow / sb_h_double_arrow /
    /// top_left_corner / top_right_corner / fleur / crosshair / X_cursor / watch）。
    /// 复验工具：`tools/verify/x11_cursor_live_probe.cpp`；协议级回归：`utest_x11_surface`
    /// 的 `AURORA_LIVE_X11=1` 用例。
    /// @warning 本 TU 会被 `<X11/X.h>` 的 `#define CursorShape 0` 宏污染（本项目类型同名），
    /// 任何引入 Xlib 的翻译单元都必须在 Xlib 头之后 `#undef CursorShape`，详见 x11_surface.cpp。
    auto set_cursor(CursorShape shape) -> void override;

    /// @brief 原生窗口句柄：X11 `Window`（XID）经 uintptr_t 装入 void*。
    [[nodiscard]] auto native_handle() const -> void * override;

    /// @brief 该窗口所在的 X 服务器连接：`Display*` 装入 void*（未连接 = nullptr）。
    /// 与 `native_handle()`（XID Window）配对使用——wgpu Xlib surface 创建（
    /// `WGPUSurfaceSourceXlibWindow`）等外部 GPU 接线需要两者同源。
    [[nodiscard]] auto native_display() const -> void *;

  private:
    struct Impl;  ///< 全部 Xlib 状态（Display/Window/GC/XImage/XIM/唤醒管道），见 x11_surface.cpp。
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX
