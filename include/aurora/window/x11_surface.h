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
    /// @brief 构造（X 连接 + 窗口创建）：三参重载以默认窗口样式转发到五参重载；不抛异常。
    /// @param w 窗口初始宽度（逻辑像素）。
    /// @param h 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    X11Surface(int w, int h, const std::string &title) : X11Surface(w, h, title, WindowStyleOptions{}) {}
    /// @brief 完整构造：连接 X 服务器（`XOpenDisplay`）并创建窗口，样式经 EWMH/_MOTIF_WM_HINTS/XSizeHints 映射。
    /// 失败（无 DISPLAY/纯 TTY）不抛异常，`is_available()` 为 false，由工厂转为 Result 错误。
    /// @param w 窗口初始宽度（逻辑像素）。
    /// @param h 窗口初始高度（逻辑像素）。
    /// @param title 窗口标题（UTF-8）。
    /// @param style 窗口样式选项（置顶/无边框/尺寸限制等）。
    /// @param visibility 初始可见性（Normal/Hidden 等）。
    X11Surface(int w, int h, const std::string &title, const WindowStyleOptions &style,
               WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 析构：销毁窗口、关闭 X 连接、释放 XImage/光标/IM 资源并关闭自唤醒管道。
    ~X11Surface() override;

    /// @brief 禁用拷贝构造：持有唯一 X 连接/窗口资源，拷贝会产生双重所有者。
    X11Surface(const X11Surface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一 X 资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    X11Surface &operator=(const X11Surface &) = delete;

    /// @brief X 连接与窗口是否创建成功（无 DISPLAY/纯 TTY 环境为 false）。
    /// 工厂 `create_window(X11Options)` 据此返回 `Result` 错误而非崩溃。
    /// @return X 连接与窗口均就绪为 true，否则 false。
    [[nodiscard]] auto is_available() const -> bool;

    /// @brief 开始一帧：清空上帧残留脏区，按真实窗口几何（XGetGeometry）对齐 Painter 缓冲并预铺浅色底色。
    /// @param width 窗口逻辑宽（dp；×scale 得物理宽，非正时回落窗口实测）。
    /// @param height 窗口逻辑高（dp）。
    /// @return 恒返回 true（尺寸非法按 1px 兜底，无失败路径）。
    [[nodiscard]] auto begin_frame(int width, int height) -> Result<bool> override;
    /// @brief 取软件绘制器：Painter 持有本帧 RGBA 缓冲，供上层组件 CPU 绘制。
    /// @return 内部 Painter 的引用（生命周期与本对象一致）。
    [[nodiscard]] auto painter() -> Painter & override;
    /// @brief 上屏当前帧：RGBA 帧缓冲按 Visual 掩码 CPU swizzle 到 X 原生像素序后 `XPutImage`。
    /// 脏区非空时仅 blit 脏矩形（一次性消费）；成功上屏计入 `frame_count()`。
    /// @return 恒返回 true（未连接/缓冲未就绪时静默跳过本次上屏，不报错）。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前帧像素（设备像素缓冲，RGBA）：DEBUG 下覆写返回 Painter 缓冲；
    /// Release（未开 `AURORA_ENABLE_DEBUG`）回落基类默认值 nullptr，使 `save_snapshot` 返回 disabled。
    /// @return 当前帧 RGBA 像素首指针；Release 构建返回 nullptr。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    /// @brief 真实窗口截图：`XGetImage` 抓窗口 → 按 Visual 掩码提取 RGBA → `write_png`。
    /// 覆写基类默认（unsupported）；Release（未开 `AURORA_ENABLE_DEBUG`）回落 unsupported 错误。
    /// @param path 输出 PNG 文件路径。
    /// @return 抓取并写盘成功返回 true；失败或 Release 下返回错误 Result。
    [[nodiscard]] auto capture_window(const std::string &path) -> Result<bool> override;
    /// @brief 窗口当前逻辑尺寸（转发 Impl 缓存的窗口几何）。
    /// @return 窗口尺寸（设备无关像素）。
    [[nodiscard]] auto size() const -> Size override;
    /// @brief 已呈现帧数：每次 `present()` 真正 `XPutImage` 上屏自增（与 Win32 同口径）。
    /// @return 迄今真正上屏的帧数。
    [[nodiscard]] auto frame_count() const -> int override;
    /// @brief begin_frame 铺的浅色底色（与 begin_frame 内 fill_rect 同色）：供脏区裁剪重绘重铺底色。
    /// @return 底色 RGBA（245,245,247,255，与通用默认一致）。
    [[nodiscard]] auto clear_color() const -> Color override { return Color{245, 245, 247, 255}; }
    /// @brief 像素密度：解析 X 资源 `Xft.dpi`（dpi/96），无声明时 1.0。
    /// @return DPI 缩放因子，1.0 表示无缩放。
    [[nodiscard]] auto scale_factor() const -> float override;
    /// @brief 是否已收到关闭窗口请求：`ClientMessage(WM_DELETE_WINDOW)` 置位。
    /// @return 收到关闭请求为 true，主循环据此退出。
    [[nodiscard]] auto should_close() const -> bool override;
    /// @brief 非阻塞抽取并派发 X 队列中的全部事件（翻译为 aurora `Event` 后上抛）。
    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待 X 事件/唤醒/超时：poll(2) 于 X 连接 fd + 自唤醒管道。
    /// @param timeout_ms 最长等待毫秒数；负值表示无限等待，0 表示立即返回。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒主循环（线程安全）：向自唤醒管道写 1 字节打断 wait_events。
    auto request_wake() -> void override;

    /// @brief 增量上屏脏区（设备坐标）：非空时 present() 仅 swizzle+XPutImage 脏矩形，
    /// 而非整窗（对齐 Win32Surface 的增量 blit 策略）；脏区一次性消费。
    /// @param device_rects 本帧脏矩形列表（设备坐标），空表示全量 blit。
    auto set_present_dirty(const std::vector<Rect> &device_rects) -> void override;

    /// @brief 事件处理器：Xlib 事件翻译为 aurora `Event` 后上抛，由 Application 统一派发。
    /// @param h 事件回调，接收翻译后的归一化事件。
    auto set_event_handler(const EventHandler &h) -> void override;
    /// @brief 运行时更新窗口标题（XStoreName + _NET_WM_NAME，UTF-8）。
    /// @param title 新窗口标题（UTF-8）。
    auto set_title(const std::string &title) -> void override;

    /// @brief 运行时更新悬停光标形状：`XCreateFontCursor` + `XDefineCursor` + `XFlush`。
    /// 句柄按 `CursorShape` 取值序缓存在 Impl（`XCreateFontCursor` 每次调用都产生新资源，
    /// 反复悬停切换必泄漏），析构统一 `XFreeCursor`。
    /// 后端映射：Arrow→`XC_left_ptr`、IBeam→`XC_xterm`、PointingHand→`XC_hand2`、ResizeNS→`XC_sb_v_double_arrow`、
    /// ResizeEW→`XC_sb_h_double_arrow`、ResizeNWSE→`XC_top_left_corner`、ResizeNESW→`XC_top_right_corner`、
    /// Move→`XC_fleur`、Crosshair→`XC_crosshair`、NotAllowed→`XC_X_cursor`、Wait→`XC_watch`。
    /// @param shape 光标语义形状，按上表映射为 Xlib 游标字形。
    /// @note 真机已验证（2026-09-13）：以 `AURORA_BACKEND_X11=ON` 编译通过；并在真实 X server
    /// 上运行时读回（XFIXES `XFixesGetCursorImage`）确认 11 个形状逐个改变了屏幕显示光标且两两互异，
    /// 名称与上表逐项吻合（left_ptr / xterm / hand2 / sb_v_double_arrow / sb_h_double_arrow /
    /// top_left_corner / top_right_corner / fleur / crosshair / X_cursor / watch）。
    /// 复验工具：`tools/verify/x11_cursor_live_probe.cpp`；协议级回归：`utest_x11_surface`
    /// 的 `AURORA_LIVE_X11=1` 用例。
    /// @warning 本 TU 会被 `<X11/X.h>` 的 `#define CursorShape 0` 宏污染（本项目类型同名），
    /// 任何引入 Xlib 的翻译单元都必须在 Xlib 头之后 `#undef CursorShape`，详见 x11_surface.cpp。
    auto set_cursor(CursorShape shape) -> void override;

    // ---- 输入法（XIM/Xlib R6 公共面）----

    /// @brief 接管组合插入点查询（Surface 契约见 surface.h）：XIM 侧用作 `XNSpotLocation`
    /// （组合串/候选窗锚点，客户窗口物理 px），组合期每次 preedit 更新与焦点切换时拉取。
    /// @param provider 返回插入点客户区矩形（物理像素）的回调，供 XIM XNSpotLocation。
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override;

    /// @brief XIM 桥的本端状态（真机验收探针的观测面：「本端与 IM 协商到什么」的物证）。
    struct ImeState {
        bool im_open = false;  ///< XOpenIM 成功（无 XIM 服务器/未设 XMODIFIERS 时 false = 纯 keysym 路径）。
        bool ic_created = false;  ///< XCreateIC 成功。
        bool preedit_callbacks =
            false;  ///< 协商到 XIMPreeditCallbacks（组合事件可回推）；false = PreeditNothing 降级。
        bool focused = false;  ///< 当前持有 X IM 焦点（XSetICFocus 已发且未 XUnsetICFocus）。
        std::string preedit;  ///< 最近一次 preedit 回调的串（UTF-8；空 = 无组合）。
        int draw_callbacks = 0;  ///< preedit draw 回调次数（含清空帧）。
        int spot_updates = 0;  ///< XNSpotLocation 实际下发次数（去重后）。
    };

    /// @brief 取 XIM 桥本端状态（探针逐项断言用；无 X 会话时全零）。
    /// @return ImeState 值拷贝：IM/IC/preedit/焦点与回调计数的当前快照。
    [[nodiscard]] auto ime_state() const -> ImeState;

    /// @brief 原生窗口句柄：X11 `Window`（XID）经 uintptr_t 装入 void*。
    [[nodiscard]] auto native_handle() const -> void * override;

    /// @brief 该窗口所在的 X 服务器连接：`Display*` 装入 void*（未连接 = nullptr）。
    /// 与 `native_handle()`（XID Window）配对使用——wgpu Xlib surface 创建（
    /// `WGPUSurfaceSourceXlibWindow`）等外部 GPU 接线需要两者同源。
    [[nodiscard]] auto native_display() const -> void *;

    /// @brief AT-SPI2 无障碍桥（`a11y::Provider`）：首次根注入前 / 降级（无 libdbus、
    /// 无会话总线、`NO_AT_BRIDGE=1`）时恒 nullptr。
    /// @return AT-SPI2 桥指针（本对象拥有；降级路径恒 nullptr）。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider * override;

    /// @brief 语义树根注入（`Window::present_root` 每帧调用；D9 宿主通道）。
    /// 首次调用即尝试建桥（dlopen libdbus + 连 a11y 总线 + Socket.Embed——AT-SPI 没有
    /// `WM_GETOBJECT` 式查询触发点，构造期连上总线是 GNOME/Qt 应用同款形态）；失败永久降级。
    /// @param root 语义树根控件指针。
    auto set_accessibility_root(Widget *root) -> void override;

    /// @brief 全部 Xlib 状态（Display/Window/GC/XImage/XIM/唤醒管道），见 x11_surface.cpp。
    /// public 而非 private：XIM preedit 回调（C 函数指针经 client_data 收发 user data）需在
    /// 类外以 `Impl*` 转发，与 Wayland 后端 C listener 同一理由。
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX
