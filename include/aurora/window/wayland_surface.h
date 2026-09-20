#pragma once
#include "aurora/core/platform.h"

// 原生 Wayland Surface（ARCHITECTURE.md §8.4）：Linux 桌面 Wayland 会话原生窗口后端。
// 仅在 defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && AURORA_BACKEND_WAYLAND 时提供；
// 依赖：wayland-client + wayland-cursor + xkbcommon + wayland-protocols（xdg-shell）。
// Debian/Ubuntu `apt install libwayland-dev libxkbcommon-dev wayland-protocols`（wayland-cursor 随 libwayland-dev）；
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

#include <cstdint>
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
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }
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

    /// @brief 运行时更新悬停光标形状——**客户端主题光标真接线**（自绘位图经 cursor
    /// `wl_surface` 提交后由 `wl_pointer.set_cursor` 交合成器接受）。
    ///
    /// Wayland 没有「服务端换光标」这回事：客户端必须自备一个 cursor `wl_surface`，把形状的
    /// ARGB 位图 attach+commit 上去，再带着**本次进入表面时的 serial** 调
    /// `wl_pointer.set_cursor`，合成器才会用它覆盖系统默认光标。本实现走
    /// `libwayland-cursor`（`wl_cursor_theme_load` 按 `24 × scale` 设备像素加载 XCursor 主题，
    /// `wl_cursor_image_get_buffer` 直接取主题自有的 ARGB `wl_buffer`，无需本端二次上传）：
    /// - 形状 → 主题名取 `cursor_rfc_name`（`window/cursor_map.h`，即 freedesktop 规范名，
    ///   与 W3C CSS `cursor` 关键字同源）；主题缺该名时回退 `default` → `left_ptr`，
    ///   三者皆缺则一次性 WARN 并保持当前光标（不隐藏系统光标）。
    /// - `set_cursor` 常在 `wl_pointer.enter` 之前被调用（首帧/无指针会话）：此时只落盘
    ///   `pending_cursor_shape`，serial 一到（`ptr_enter`）立即补下发，故无需调用方重试。
    /// - 每次 `enter` 都强制重下发：合成器在指针重新进入时回到默认光标，去重只做在「同一
    ///   焦点期内同形状同缩放」。
    /// - 缩放变化（`wl_output.scale`）→ 重载主题并按新尺寸重下发。
    /// - 动画光标（如 `wait`）取首帧静态图，不做逐帧定时重提交：合成器侧的动画光标由主题
    ///   自身决定，本端不模拟（已知限制）。
    /// 备选更省路径 `wp_cursor_shape_manager_v1`（免自管 buffer）需合成器提供该扩展，本机
    /// WSLg Weston 未发布（实测 registry globals 无之），故不走该路。
    /// @note 读回口径：Wayland 客户端**无任何 API 可查询「屏幕上当前显示的光标」**（不同于
    /// Win32 `GetCursorInfo` / X11 XFIXES）。可机器判定的只有本端提交了什么，见 `cursor_state()`
    /// 与 `tools/verify/wayland_cursor_live_probe.cpp`；「屏幕像素确已改变」不在证明范围内。
    auto set_cursor(CursorShape shape) -> void override;

    /// @brief `set_cursor` 的本端提交状态（真机验收探针的观测面，见 `set_cursor` 的读回口径）。
    ///
    /// 全部字段都是「本进程向合成器提交了什么」的物证，而非屏幕读回；未连接/无指针/主题缺失
    /// 时 `applied` 为 false 且其余字段保持初值（不抛、不崩，便于无头环境安全调用）。
    struct CursorState {
        bool applied = false;  ///< 至少成功提交过一次（cursor 表面 commit + `wl_pointer_set_cursor`）。
        bool pointer_entered = false;  ///< 是否收到过 `wl_pointer.enter`（无 enter 则无合法 serial）。
        CursorShape shape = CursorShape::Arrow;  ///< 最近一次成功提交的语义形状。
        std::string resolved_name;  ///< 主题侧实际命中的光标名（`wl_cursor::name`，可与请求名不同）。
        int image_width = 0;  ///< 命中图像宽（设备像素；0 = 未命中）。
        int image_height = 0;  ///< 命中图像高（设备像素）。
        int buffer_scale = 1;  ///< cursor 表面的 `set_buffer_scale`（图像非缩放整数倍时退化 1）。
        int hotspot_x = 0;  ///< 热点（表面逻辑坐标，已按 `buffer_scale` 折算）。
        int hotspot_y = 0;
        std::uint64_t buffer_id = 0;  ///< 提交的 `wl_buffer` 身份（主题持有；不同形状通常不同）。
        int image_count = 0;  ///< 该光标的动画帧数（>1 即动画光标，本端只取首帧）。
        int theme_size = 0;  ///< 主题加载尺寸（设备像素 = 24 × scale）。
        int commits = 0;  ///< cursor 表面 commit 次数（去重后每次形状/缩放变化 +1，同形状幂等不加）。
    };

    /// @brief 取 `set_cursor` 的本端提交状态（探针逐项断言用；无 Wayland 会话时全零）。
    [[nodiscard]] auto cursor_state() const -> CursorState;

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

    /// @brief 该窗口所在的 Wayland 合成器连接：`wl_display*`（未连接 = nullptr）。
    /// 与 `native_handle()`（wl_surface*）配对使用——wgpu Wayland surface 创建（
    /// `WGPUSurfaceSourceWaylandSurface`）等外部 GPU 接线需要两者同源。
    [[nodiscard]] auto native_display() const -> void *;

    /// @brief AT-SPI2 无障碍桥（`a11y::Provider`）：首次根注入前 / 降级（无 libdbus、
    /// 无会话总线、`NO_AT_BRIDGE=1`）时恒 nullptr。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override;

    /// @brief 语义树根注入（`Window::present_root` 每帧调用；D9 宿主通道）。
    /// 首次调用即尝试建桥（dlopen libdbus + 连 a11y 总线 + Socket.Embed）；失败永久降级。
    /// @note 申报偏差：xdg-shell 不暴露窗口屏幕原点 ⇒ 几何按窗口本地 px 申报。
    auto set_accessibility_root(Widget *root) -> void override;

    /// @brief 是否正在自绘 CSD 装饰（标题栏/边框，画进 Painter 帧缓冲）：合成器无
    /// xdg-decoration SSD 且装饰策略需要兜底时为 true。GPU 宿主（WgpuWaylandSurface）
    /// 据此决定是否需要把装饰录制进当帧。
    [[nodiscard]] auto uses_client_decorations() const -> bool;

    /// @brief 把本帧 CSD 自绘装饰**录制**为 `DisplayList`（不触帧缓冲），供 GPU 宿主追加
    /// 回放进当帧——swapchain 独占 `wl_surface`，`present()` 里的软件光栅上不了屏，故装饰
    /// 必须走命令通道。绘制内容与软件路径逐命令同源（`csd::paint_title_bar` 单一实现）。
    ///
    /// 坐标为**逻辑 dp**（与帧级 DL 同口径，缩放在回放侧生效）。本帧无装饰可画（无 CSD
    /// 标题栏、或全屏且未揭示顶边条）时返回 false 且不清空/不改写 `dl`，调用方据此跳过回放。
    /// @note 用后端自带的独立录制 Painter，可在 app 帧 DL 录制期间安全调用（互不嵌套）。
    auto record_client_decoration(DisplayList &dl) -> bool;

    /// @brief 全部 Wayland/xkb 状态（display/registry/shm 双缓冲/seat/唤醒管道），见 wayland_surface.cpp。
    /// public 而非 private：C 协议 listener（自由函数指针表）需在类外以 `Impl*` 收发 user data。
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_WAYLAND / AURORA_PLATFORM_LINUX
