#pragma once
#include "aurora/core/platform.h"

/// @brief macOS AppKit/CoreGraphics Surface（ARCHITECTURE.md §8.4）：仅在 defined(AURORA_PLATFORM_MACOS) 时提供。
/// 其他平台降级为 HeadlessSurface。
/// @file macos_surface.h

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <cstdint>
#include <memory>
#include <string>

#include "aurora/window/surface.h"

namespace aurora {

/// @brief macOS AppKit Surface：NSWindow + 自定义 NSView，软件 Painter 帧缓冲经 CGImage 上屏。
///
/// 实现面（与 Win32 / X11 后端同口径）：
/// - 窗口壳：`NSWindowStyleMaskTitled|Closable|Miniaturizable`（可缩放时再加 `Resizable`），
///   Frameless / 无边框档改 `Borderless`；标题、尺寸、位置、置顶、透明、最小/最大尺寸在构造期
///   按 `WindowStyleOptions` 映射；
/// - 上屏：`Painter` 的 RGBA 物理像素缓冲包装为 `CGImage`，在视图 `drawRect:` 内 1:1 绘制
///   （Retina 下物理像素与 backing store 对齐，无缩放模糊）；
/// - 事件：鼠标（左/右/中、移动、拖拽）、滚轮、键盘、修饰键、文本输入与 IME 组合、
///   窗口关闭/缩放/最小化/全屏/激活/屏幕切换，翻译为 `aurora::Event` 后交宿主派发器；
/// - 帧循环：`poll_platform_events()` 抽干 `NSApplication` 队列，`wait_events()` 以
///   `nextEventMatchingMask:untilDate:inMode:dequeue:` 阻塞到事件或超时；队列为进程级，
///   故 `pumps_thread_queue()` / `waits_thread_queue()` 均为 true；
/// - DPI：`backingScaleFactor`（Retina 通常 2.0）为唯一真值点，跨屏/缩放变更经
///   `windowDidChangeBackingProperties:` 上报 `notify_scale_change`。
///
/// @note Thread: main-thread only（AppKit 契约：窗口与事件 API 只在主线程调用）。
/// @note Side-effects: 创建/销毁原生窗口，读写系统剪贴板类资源以外的窗口状态。
/// @note Rebuildable: no
class MacOSSurface : public Surface {
  public:
    /// @brief 构造并创建原生窗口（立即 `makeKeyAndOrderFront:`；Hidden 档除外）。
    /// @param w     初始内容区宽（逻辑 dp / AppKit point）。
    /// @param h     初始内容区高（逻辑 dp / AppKit point）。
    /// @param title 窗口标题（UTF-8；经 `NSString stringWithUTF8String:` 设置）。
    /// @param style 高级样式（装饰 / 置顶 / 透明 / 尺寸限制；见 `WindowStyleOptions`）。
    /// @param visibility 可见性策略：Normal 显示并激活、NoActivate 显示不激活、Hidden 只建窗不入视野。
    MacOSSurface(int w, int h, const std::string &title, const WindowStyleOptions &style = WindowStyleOptions{},
                 WindowVisibility visibility = WindowVisibility::Normal);
    /// @brief 析构：先收摊系统拖放桥（在途会话按取消兑现），再释放窗口/视图/委托。
    ~MacOSSurface() override;

    /// @brief 禁用拷贝构造：持有唯一窗口壳与整帧 Painter，拷贝会产生双重所有者。
    MacOSSurface(const MacOSSurface &) = delete;
    /// @brief 禁用拷贝赋值：同拷贝构造，唯一窗口/Painter 资源不可复制。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(const MacOSSurface &) -> MacOSSurface & = delete;
    /// @brief 禁用移动：窗口壳生命周期与对象同寿，资源无「移交给新属主」的语义。
    MacOSSurface(MacOSSurface &&) = delete;
    /// @brief 禁用移动赋值：同移动构造，唯一资源不可移交。
    /// @return 已删除重载，不存在实际返回路径。
    auto operator=(MacOSSurface &&) -> MacOSSurface & = delete;

    /// @brief 开启新一帧：按当前内容区尺寸（逻辑 dp）重建/复用 Painter，并以浅色底整帧填充。
    ///
    /// 尺寸以原生内容视图的真实 bounds 为准（窗口 resize 通知后调用方传参可能滞后一帧），
    /// 与 X11 / GLFW 的「缓冲 1:1 贴窗口」口径一致；视图缺失时回退到入参。
    /// @param w 帧宽（逻辑 dp）。
    /// @param h 帧高（逻辑 dp）。
    /// @return 软件帧缓冲路径恒成功。
    [[nodiscard]] auto begin_frame(int w, int h) -> Result<bool> override;
    /// @brief 取得整帧共享的软件绘制器。
    /// @return 内部 Painter 引用（生命周期随本 Surface）。
    [[nodiscard]] auto painter() -> Painter & override;
    /// @brief 提交当前帧：RGBA 缓冲 → CGImage → 视图重绘（同步 `displayIfNeeded`）。
    /// @return 成功 true；无窗口/无缓冲时如实报失败。
    [[nodiscard]] auto present() -> Result<bool> override;
    /// @brief 当前内容区尺寸（逻辑 dp）。
    /// @return 尺寸；窗口已销毁时为最近一次值。
    [[nodiscard]] auto size() const -> Size override;
    /// @brief 设备像素密度（`backingScaleFactor`）。
    /// @return 缩放因子；无窗口时回退 1.0。
    [[nodiscard]] auto scale_factor() const -> float override;
    /// @brief 是否收到关闭请求（`windowShouldClose:` 或程序化 `close()` 置位）。
    /// @return 已请求关闭 true；窗口壳不存在时亦为 true（宿主据此回收）。
    [[nodiscard]] auto should_close() const -> bool override;
    /// @brief 撤销关闭请求：清 Impl 标志（窗口仍存活时）。`windowShouldClose:` 返回 NO 后
    ///        由 `Application` 的关闭闸门在否决分支调用。
    auto cancel_close() -> void override;
    /// @brief 轮询并派发 `NSApplication` 队列内已就绪的事件（不阻塞）。
    auto poll_platform_events() -> void override;
    /// @brief 阻塞等待平台事件或超时：队列级等待（任一窗口的事件都会唤醒）。
    /// @param timeout_ms 等待上限毫秒；<0 无限等待；==0 立即返回。
    auto wait_events(double timeout_ms) -> void override;
    /// @brief 跨线程唤醒：向 `NSApplication` 队列投递一个应用自定义空事件。
    auto request_wake() -> void override;
    /// @brief 本后端抽干进程级 `NSApplication` 事件队列。
    /// @return 恒 true。
    [[nodiscard]] auto pumps_thread_queue() const -> bool override;
    /// @brief 本后端的等待通道覆盖任意窗口（`NSApplication` 队列级）。
    /// @return 恒 true。
    [[nodiscard]] auto waits_thread_queue() const -> bool override;

    /// @brief 事件处理器：AppKit 事件（含系统拖放四态）翻译为 aurora `Event` 后上抛。
    /// @param h 事件接收器；为空时事件仅更新内部状态、不上抛。
    auto set_event_handler(const EventHandler &h) -> void override;

    /// @brief 运行时更新窗口标题。
    /// @param title UTF-8 标题。
    auto set_title(const std::string &title) -> void override;
    /// @brief 程序化设置内容区尺寸（逻辑 dp）。
    /// @param s 目标尺寸（逻辑 dp）。
    auto set_size(Size s) -> void override;
    /// @brief 程序化移动窗口（屏幕物理像素，与 `Display::bounds` 同坐标系）。
    /// @param p 目标左上角（物理像素，原点为主屏左上角）。
    auto set_position(Point p) -> void override;
    /// @brief 当前窗口左上角屏幕坐标（物理像素）。
    /// @return 位置；无窗口时 (0,0)。
    [[nodiscard]] auto position() const -> Point override;
    /// @brief 当前窗口所在显示器稳定 id（`CGDirectDisplayID`）。
    /// @return 显示器 id；无窗口时 -1。
    [[nodiscard]] auto display_id() const -> int override;
    /// @brief 程序化最小化。
    auto minimize() -> void override;
    /// @brief 程序化切换最大化（macOS「缩放」语义：铺满当前屏幕工作区）。
    auto toggle_maximize() -> void override;
    /// @brief 进入/退出系统全屏（`toggleFullScreen:`）。
    /// @param on true 进入全屏，false 退出。
    auto set_fullscreen(bool on) -> void override;
    /// @brief 设定窗口几何态（目标态语义，幂等），并同步上报 `notify_window_mode`。
    /// @param m 目标几何态。
    auto set_window_mode(WindowMode m) -> void override;
    /// @brief 程序化请求关闭（置关闭标志，不销毁窗口；由宿主回收时析构）。
    auto close() -> void override;
    /// @brief 把窗口提到同级 z 序顶部（不激活）。
    auto raise() -> void override;
    /// @brief 激活窗口并取得键盘焦点。
    auto focus_window() -> void override;
    /// @brief 建立/解除与 owner 的 OS 从属关系（子窗口随 owner 移动/最小化）。
    /// @param owner 父窗口 Surface；nullptr 表示解除。
    auto set_owner(const Surface *owner) -> void override;
    /// @brief 启用/禁用窗口接收鼠标输入（模态屏蔽 owner 时使用）。
    /// @param on true 启用，false 禁用。
    auto set_enabled(bool on) -> void override;
    /// @brief 控件发起窗口拖拽移动（`performWindowDragWithEvent:`，须在鼠标按下派发栈内调用）。
    auto begin_window_move() -> void override;
    /// @brief 运行时更新悬停光标形状。
    /// @param shape 待应用的光标语义形状。
    auto set_cursor(CursorShape shape) -> void override;

    /// @brief 注册立即重绘请求回调（窗口几何变化时触发宿主补一帧）。
    /// @param h 请求回调。
    auto set_present_request(PresentRequest h) -> void override;
    /// @brief 帧像素只读指针（RGBA8）。
    /// @return Painter 缓冲首地址；尚未 begin_frame 时 nullptr。
    [[nodiscard]] auto data() const -> const std::uint8_t * override;
    /// @brief 帧缓冲物理像素尺寸。
    /// @return 物理宽高（Retina 下为逻辑尺寸 × scale）。
    [[nodiscard]] auto framebuffer_size() const -> Size override;
    /// @brief 原生窗口句柄（`NSWindow *`，以 `void *` 承载）。
    [[nodiscard]] auto native_handle() const -> void * override;
    /// @brief 已成功上屏的帧数。
    /// @return `present()` 成功次数。
    [[nodiscard]] auto frame_count() const -> int override;
    /// @brief 本后端 `begin_frame` 铺的底色（窗口内容区背景）。
    /// @return #F5F5F7 不透明色。
    [[nodiscard]] auto clear_color() const -> Color override;
    /// @brief 注入 IME 候选窗定位查询（窗口逻辑 dp，零宽竖盒）。
    /// @param provider 候选窗定位查询；空表示无定位。
    auto set_composition_caret_provider(std::function<Rect()> provider) -> void override;

    /// @brief 全部 AppKit 状态（NSWindow/NSView/委托/CGImage/拖放桥/修饰位缓存）。
    ///
    /// public 而非 private：ObjC 视图与委托类（`NSWindowDelegate` / `NSTextInputClient`
    /// 实现）需在类外以 `Impl *` 还原 user data 后转发，与 Wayland / X11 后端的 C listener
    /// 同一处置。所有权仍由 `impl_` 唯一持有。
    struct Impl;

  private:
    /// @brief Pimpl 状态（定义于 .cpp）。
    std::unique_ptr<Impl> impl_;
    /// @brief 整帧软件绘制器（begin_frame 重置、供 present 读取 RGBA 缓冲）。
    Painter painter_;
    /// @brief 最近一次约定（构造/begin_frame/缩放通知）的帧尺寸（逻辑 dp）。
    Size size_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
