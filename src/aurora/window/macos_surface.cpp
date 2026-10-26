// macos_surface.cpp — macOS AppKit/CoreGraphics Surface 实现骨架。
// 仅在 defined(AURORA_PLATFORM_MACOS) && AURORA_BACKEND_MACOS 时编译。
//
// 设计要点（待 macOS 环境实写）：
// - NSWindow + NSView 自定义视图（覆盖 drawRect: 用 CGContextDrawImage 上屏）。
// - 上屏路径：软件 Painter RGBA 帧缓冲 → CGDataProvider 包装 → CGImage → CGContextDrawImage。
//   RGBA 无需 swizzle（CGImage 支持 kCGImageAlphaPremultipliedLast = RGBA）。
// - 事件翻译：NSView 的 mouseDown/Up/Moved/keyDown/Up/flagsChanged/scrollWheel 翻译为 aurora Event。
// - 帧循环：CVDisplayLink 或 dispatch_source_t 定时器驱动；macOS 14+ 推荐 CADisplayLink。
// - DPI：NSWindow backingScaleFactor（Retina 2x/3x）→ scale_factor()。
// - 关闭语义：windowShouldClose: → should_close_ = true。
// - 系统拖放：内容视图的 `NSDraggingDestination` 五方法与拖出的 `NSDraggingSource` 全部落在
//   `detail::MacOSDragBridge`（会话态与时序在桥内）；表层只做「安装桥 + 暂存按下事件」。
//
// 本文件为骨架，全部方法在 macOS 环境实写后替换 stub。

#include "aurora/core/platform.h"  // NOLINT
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include "aurora/core/log.h"
#include "aurora/window/detail/macos_drag.h"
#include "aurora/window/macos_surface.h"

// AppKit 头需在 .cpp 内引入，避免公共头污染消费者
#import <AppKit/AppKit.h>

// `busyButClickableCursor` 是 NSCursor 的私有类方法（无公开声明）。补一个类别声明，
// 让编译器把返回类型识别成 `NSCursor *`（消 -Wobjc-method-access），`[cursor set]` 也因而
// 落到 NSCursor 唯一的 `set` 上（消 -Wobjc-multiple-method-names）。仅本文件 macOS 实现内使用。
@interface NSCursor (AuroraPrivateCursor)
+ (NSCursor *)busyButClickableCursor;
@end

namespace aurora {

struct MacOSSurface::Impl {
    NSWindow *window = nullptr;
    NSView *view = nullptr;  ///< 自定义 NSView 子类实例（覆盖 drawRect:）
    bool should_close = false;
    float scale = 1.0F;
    Surface::EventHandler event_handler;
    /// @brief 系统拖放桥（契约与会话状态机见 `detail::MacOSDragBridge`），本 Impl 唯一持有。
    ///
    /// 骨架期窗口壳未实写 ⇒ `view` 为空 ⇒ `install()` 必然失败并降级为空（见 `install_drag`）。
    /// 事件泵落地后另有三处调用点必须补上，缺一则相应方向不通（与 Wayland 桥同款纪律）：
    ///   ① 泵入口 `note_initiator(this)`：拖出由控件在事件派发期内发起，发起窗口即本窗口；
    ///   ② 帧末 `drag->tick()`：macOS 侧为空转（取数同步发生在 perform 内），仅为同形调用点；
    ///   ③ `mouseDown:` 派发期内 `drag->note_press_event(event)`：`beginDraggingSession` 的必需实参。
    std::unique_ptr<detail::MacOSDragBridge> drag;

    /// @brief 安装系统拖放桥（窗口壳建好之后调用：登记与发起都以内容视图为落点）。
    ///
    /// 失败即本窗口不参与系统拖放，如实记日志降级、不谎报已有通路（与 X11 / Wayland 同款处置）。
    auto install_drag() -> void {
        if (view == nullptr) {
            // 骨架期无视图可登记（`create_window` 未实写）：不构造桥、不注册进程级后端。
            AURORA_LOG_INFO("window", "MacOSSurface: no content view; system drag not installed.");
            return;
        }
        drag = std::make_unique<detail::MacOSDragBridge>(
            view, detail::MacOSDragBridge::Hooks{
                      .emit = [this](Event &e) -> void {
                          if (event_handler != nullptr) {
                              event_handler(e);  // 派发器在调用内写定 `e.allowed`（Enter / Over 两态）
                          }
                      },
                      .scale_factor = [this]() -> float { return scale; },
                      // 骨架期无键盘路径缓存（键盘事件翻译未实写）⇒ 如实报「不知道」，不猜修饰态。
                      .modifiers = []() -> ModifierKey { return ModifierKey::None; }});
        if (!drag->install()) {
            drag.reset();
            AURORA_LOG_WARN("window", "MacOSSurface: drag bridge install failed; system drag unavailable.");
        }
    }

    /// @brief 创建 NSWindow + 自定义 NSView。
    bool create_window(int w, int h, const std::string &title) {
        // TODO(macOS): 实写 NSWindow 创建逻辑
        // 1. NSRect frame = NSMakeRect(0, 0, w, h)
        // 2. window = [[NSWindow alloc] initWithContentRect:frame styleMask:... backing:... defer:NO]
        // 3. [window setTitle:[NSString stringWithUTF8String:title.c_str()]]
        // 4. 创建自定义 NSView 子类（覆盖 drawRect:/acceptsFirstResponder/mouseDown: 等）
        //    拖入五方法可直接复用 `MacOSDragBridge::make_drag_destination_view(this)` 产出的视图，
        //    亦可让本类继承它后叠加绘制与鼠标派发（桥只占 `NSDraggingDestination` 五方法）。
        // 5. [window setContentView:view]
        // 6. [window makeKeyAndOrderFront:nil]
        // 7. scale = [window backingScaleFactor]
        (void)w;
        (void)h;
        (void)title;
        AURORA_LOG_INFO("macos_surface", "create_window stub (implement for macOS)");
        // 拖放桥的安装点：视图就位之后（拖入登记与拖出发起都以它为落点）。
        install_drag();
        return true;
    }

    void destroy_window() {
        // 桥先收摊：它持会话对象并在 `uninstall` 内兑现「在途拖出按 Cancelled」，必须早于
        // 视图 / 窗口释放（与 WaylandSurface 析构次序同款纪律）。
        drag.reset();
        // TODO(macOS): [window close]; window = nil; view = nil;
    }
};

MacOSSurface::MacOSSurface(int w, int h, const std::string &title)
    : impl_(std::make_unique<Impl>()), size_{.width = static_cast<float>(w), .height = static_cast<float>(h)} {
    if (!impl_->create_window(w, h, title)) {
        AURORA_LOG_WARN("macos_surface", "create_window failed");
    }
}

MacOSSurface::~MacOSSurface() {
    if (impl_) {
        impl_->destroy_window();
    }
}

auto MacOSSurface::begin_frame(int w, int h) -> Result<bool> {
    painter_.begin(w, h);
    size_ = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    // 浅色底色（与 Win32/X11 一致）
    painter_.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                            .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                       Color{245, 245, 247, 255});
    return true;
}

auto MacOSSurface::painter() -> Painter & { return painter_; }

auto MacOSSurface::present() -> Result<bool> {
    // TODO(macOS): 实写上屏
    // 1. 从 painter_.data() 获取 RGBA 帧缓冲
    // 2. CGDataProviderCreateWithCFData 或 CGDataProviderCreate 包装
    // 3. CGImageCreate(w, h, 8, 32, stride, colorspace, kCGImageAlphaPremultipliedLast, provider, ...)
    // 4. 在 NSView drawRect: 中 [ctx drawImage:cgImage inRect:viewBounds]
    // 5. CGImageRelease
    // 注意：macOS 的 NSView 可能异步 drawRect:，需要 setNeedsDisplay: 触发
    AURORA_LOG_DEBUG("macos_surface", "present stub (implement for macOS)");
    return true;
}

auto MacOSSurface::size() const -> Size { return size_; }

auto MacOSSurface::should_close() const -> bool { return impl_ ? impl_->should_close : true; }
auto MacOSSurface::cancel_close() -> void {
    // impl_ 为空时 should_close() 恒真（窗口已不存在）⇒ 不改写，由 fail-open 后置复验兜住。
    if (impl_ != nullptr) {
        impl_->should_close = false;
    }
}

auto MacOSSurface::set_event_handler(const EventHandler &h) -> void {
    if (impl_ != nullptr) {
        impl_->event_handler = h;
    }
}

auto MacOSSurface::set_cursor(CursorShape shape) -> void {
    // 光标形状：NSCursor 为进程级单例，`set` 即改当前光标，无需持有/释放。
    // 须在主线程调用（AppKit 契约）；本方法由事件派发栈（主线程）调用。
    switch (shape) {
        case CursorShape::Arrow:
            [[NSCursor arrowCursor] set];
            break;
        case CursorShape::IBeam:
            [[NSCursor IBeamCursor] set];
            break;
        case CursorShape::PointingHand:
            [[NSCursor pointingHandCursor] set];
            break;
        case CursorShape::ResizeNS:
            [[NSCursor resizeUpDownCursor] set];
            break;
        case CursorShape::ResizeEW:
            [[NSCursor resizeLeftRightCursor] set];
            break;
        case CursorShape::Move:
            [[NSCursor openHandCursor] set];
            break;
        case CursorShape::Crosshair:
            [[NSCursor crosshairCursor] set];
            break;
        case CursorShape::NotAllowed:
            [[NSCursor operationNotAllowedCursor] set];
            break;
        case CursorShape::Wait:
            [[NSCursor busyButClickableCursor] set];
            break;
        case CursorShape::ResizeNWSE:
        case CursorShape::ResizeNESW:
            // macOS 无公开的对角缩放光标（仅有 private `_windowResize*` 系列），回退默认箭头。
            [[NSCursor arrowCursor] set];
            break;
    }
}

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
