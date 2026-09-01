#pragma once
#include "aurora/core/platform.h"

// 原生 Wayland Surface（ARCHITECTURE.md §8.4）：Linux 桌面 Wayland 会话原生窗口后端。
// 仅在 defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && AURORA_BACKEND_WAYLAND 时提供；
// 依赖：wayland-client + xkbcommon + wayland-protocols（xdg-shell）。
// Debian/Ubuntu `apt install libwayland-dev libxkbcommon-dev wayland-protocols`；
// Fedora `dnf install wayland-devel libxkbcommon-devel wayland-protocols-devel`。
//
// 设计要点：
// - pimpl 隔离：公共头不含 <wayland-client.h> 与 scanner 生成头（xdg-shell 胶水仅
//   存在于 build 目录），全部平台逻辑在 src/aurora/window/wayland_surface.cpp。
// - 上屏路径：软件 Painter RGBA 帧缓冲 → CPU swizzle 到 WL_SHM_FORMAT_XRGB8888
//   （小端 BGRX，与 Win32 RGBA→BGRA 等价）→ wl_shm 共享内存 wl_buffer →
//   attach + damage_buffer + commit（双缓冲槽轮换，busy 时 roundtrip 等 release）。
// - 窗口壳：wl_surface + xdg_surface + xdg_toplevel；configure 驱动尺寸/状态
//   （maximized/fullscreen/activated）；close 事件 → should_close。
//   服务端装饰经 zxdg_decoration_manager_v1 协商（KDE 有；GNOME 无 → 无标题栏，
//   frameless 语义等价，属合成器限制而非缺陷）。
// - 事件翻译：wl_pointer → MouseEvent/ScrollEvent；wl_keyboard 经 xkbcommon
//   keymap → KeyEvent + TextInputEvent（xkb_state_key_get_utf8）。
// - 帧循环：wait_events 经 poll(2) 阻塞在 wl_display fd + 自唤醒管道
//   （prepare_read/read_events 单线程范式）；request_wake 线程安全。
// - 构造不抛异常：连接失败（无 WAYLAND_DISPLAY/纯 TTY）时 is_available() 为 false，
//   工厂据此返回 Result 错误（错误归属调用方，AI 可枚举）。

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)

#include <memory>
#include <string>

#include "aurora/window/surface.h"

namespace aurora {

/// @brief 原生 Wayland 后端：软件 Painter 帧缓冲经 wl_shm 上屏，Wayland 事件翻译为 aurora `Event`。
///
/// 一帧生命周期与其他后端一致：`begin_frame` → `painter()` 绘制 → `present`。
/// 窗口样式映射：maximize/fullscreen 经 xdg_toplevel 请求；min/max 尺寸经
/// xdg_toplevel_set_min/max_size；服务端装饰按合成器能力协商。
/// @note Thread: main-thread only（request_wake 除外，线程安全）
class WaylandSurface final : public Surface {
  public:
    WaylandSurface(int w, int h, const std::string &title) : WaylandSurface(w, h, title, WindowStyleOptions{}) {}
    WaylandSurface(int w, int h, const std::string &title, const WindowStyleOptions &style);
    ~WaylandSurface() override;

    WaylandSurface(const WaylandSurface &) = delete;
    WaylandSurface &operator=(const WaylandSurface &) = delete;

    /// @brief Wayland 连接与窗口壳是否创建成功（无 WAYLAND_DISPLAY/纯 TTY 环境为 false）。
    /// 工厂 `create_window(WaylandOptions)` 据此返回 `Result` 错误而非崩溃。
    [[nodiscard]] auto is_available() const -> bool;

    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    [[nodiscard]] auto painter() -> Painter & override;
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前帧像素（设备像素缓冲，RGBA）：DEBUG 下覆写返回 Painter 缓冲；
    /// Release（未开 `AURORA_ENABLE_DEBUG`）回落基类默认值 nullptr，使 `save_snapshot` 返回 disabled。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    [[nodiscard]] auto size() const -> Size override;
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{ 245, 245, 247, 255 }; }
    /// @brief 像素密度：wl_output scale（整数缩放），多屏取窗口所在输出。
    [[nodiscard]] auto scale_factor() const -> float override;
    [[nodiscard]] auto should_close() const -> bool override;
    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待 Wayland 事件/唤醒/超时：poll(2) 于 wl_display fd + 自唤醒管道。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒主循环（线程安全）：向自唤醒管道写 1 字节打断 wait_events。
    auto request_wake() -> void override;

    /// @brief 增量上屏脏区（设备坐标）：非空时 present() 仅 swizzle+damage_buffer 脏矩形，
    /// 而非整窗（对齐 Win32/X11 的增量 blit 策略）；脏区一次性消费。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override;

    /// @brief 事件处理器：Wayland 事件翻译为 aurora `Event` 后上抛，由 Application 统一派发。
    auto set_event_handler(const EventHandler &h) -> void override;
    /// @brief 运行时更新窗口标题（xdg_toplevel_set_title，UTF-8）。
    auto set_title(const std::string &title) -> void override;
    /// @brief 运行期更新 CSD 标题栏样式（存入 Impl 并触发重绘，下帧 draw_decoration 生效）。
    auto set_title_bar_style(const TitleBarStyle &style) -> void override;
    /// @brief 控件发起窗口拖拽移动（Wayland：xdg_toplevel_move，须在 Press 派发栈内调用）。
    auto begin_window_move() -> void override;
    /// @brief 控件发起窗口边缘缩放（Wayland：xdg_toplevel_resize）。
    auto begin_window_resize(WindowResizeEdge edge) -> void override;
    /// @brief 运行期更新 CSD 标题栏图标（shared_ptr 共享像素避免深拷贝）并触发重绘。
    auto set_title_bar_icon(const std::shared_ptr<Image> &icon) -> void override;
    /// @brief 客户端装饰安全区内边距：CSD 标题栏高度（顶）与可缩放边框厚度（四周）。
    [[nodiscard]] auto content_inset() const -> EdgeInsets override;
    /// @brief 程序化关闭：置 close_requested，下帧退出主循环。
    auto close() -> void override;
    /// @brief 程序化最小化：xdg_toplevel_set_minimized。
    auto minimize() -> void override;
    /// @brief 程序化切换最大化：按当前 mode 调 set/unset_maximized。
    auto toggle_maximize() -> void override;
    /// @brief 程序化全屏：xdg_toplevel_set/unset_fullscreen。
    auto set_fullscreen(bool on) -> void override;
    /// @brief 原生窗口句柄：`wl_surface*`。
    [[nodiscard]] auto native_handle() const -> void * override;

    /// @brief 全部 Wayland/xkb 状态（display/registry/shm 双缓冲/seat/唤醒管道），见 wayland_surface.cpp。
    /// public 而非 private：C 协议 listener（自由函数指针表）需在类外以 `Impl*` 收发 user data。
    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace aurora

#endif // AURORA_BACKEND_WAYLAND / AURORA_PLATFORM_LINUX
