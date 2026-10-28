// macos_surface.cpp — macOS AppKit/CoreGraphics Surface 实现。
// 仅在 defined(AURORA_PLATFORM_MACOS) && AURORA_BACKEND_MACOS 时编译。
//
// 设计要点（与头文件契约同源）：
// - NSWindow + 自定义 NSView（翻转坐标，覆盖 drawRect: 用 CGContextDrawImage 上屏）。
// - 上屏路径：软件 Painter RGBA 帧缓冲零拷贝包装为 CGImage（数据指针指向 Painter 缓冲，
//   缓冲换址前必须释放旧 CGImage —— begin_frame 在重分配前执行该释放）。
// - 事件翻译：视图的 mouseDown/Up/Moved/rightMouse/otherMouse、scrollWheel、keyDown/Up、
//   flagsChanged 翻译为 aurora Event；文本输入与 IME 走 NSTextInputClient 协议。
// - 帧循环：NSApplication 事件队列即进程级队列，poll/wait 均抽干本队列并按窗口路由。
// - DPI：NSWindow.backingScaleFactor（Retina 通常 2.0）→ scale_factor()。
// - 关闭语义：windowShouldClose: → should_close = true 并返回 NO（窗口回收由框架决定）。
// - 系统拖放：内容视图的 NSDraggingDestination 五方法转发到 detail::MacOSDragBridge。
//
// 内存纪律：**MRC**（无 -fobjc-arc，与 macos_drag.cpp 同口径）。ObjC 对象显式
// retain/release；窗口、委托与 CGImage 的所有权由 Impl 唯一持有。

#include "aurora/core/platform.h"  // NOLINT
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/detail/ime_composition.h"
#include "aurora/window/detail/macos_drag.h"
#include "aurora/window/detail/macos_keymap.h"
#include "aurora/window/detail/macos_menu_bar.h"
#include "aurora/window/detail/macos_recent_documents.h"
#include "aurora/window/macos_surface.h"
#include "aurora/window/platform.h"  // os_color_scheme 的 macOS 实现（detail::macos_os_color_scheme）

// AppKit 头需在 .cpp 内引入，避免公共头污染消费者
#import <AppKit/AppKit.h>

// `busyButClickableCursor` 是 NSCursor 的私有类方法（无公开声明）。补一个类别声明，
// 让编译器把返回类型识别成 `NSCursor *`（消 -Wobjc-method-access），`[cursor set]` 也因而
// 落到 NSCursor 唯一的 `set` 上（消 -Wobjc-multiple-method-names）。仅本文件 macOS 实现内使用。
@interface NSCursor (AuroraPrivateCursor)
+ (NSCursor *)busyButClickableCursor;
@end

// ObjC 方法体位于全局作用域（`@implementation` 不能置于 namespace 内），事件类型在此以
// using 声明引入，避免 ObjC 方法里逐处书写 `aurora::` 前缀。`Point` 不在此列——它与
// Apple SDK 的常用同名类型撞面大，滚动事件处单独全限定。
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::ModifierKey;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::ScrollEvent;
using aurora::TextCompositionEvent;
using aurora::TextInputEvent;

// =============================================================================
// ObjC 类声明（user data 一律是 Impl *，方法实现见下方）
// =============================================================================
// 声明须先于 `Impl` 定义：`Impl::create_window` 在类体内直接 alloc 这两个类。ivars 指向
// 公共头已前置声明的嵌套类型 `MacOSSurface::Impl`（此处仍是 incomplete type，指针足够）。

// 内容视图：翻转坐标、接管鼠标/键盘/滚轮，并把拖入五方法转发给拖放桥。
@interface AuroraMacSurfaceView : NSView <NSTextInputClient> {
  @public
    aurora::MacOSSurface::Impl *impl;
}
// 以 Impl 回指构造（视图不拥有 Impl）；frame 为初始内容区，impl_ 为所属 Surface 的状态。
- (id)initWithFrame:(NSRect)frame impl:(aurora::MacOSSurface::Impl *)impl_;
@end

// 窗口委托：关闭闸门与状态变化 → Impl 上报。
@interface AuroraMacWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    aurora::MacOSSurface::Impl *impl;
}
// 以 Impl 回指构造（委托不拥有 Impl）；impl_ 为所属 Surface 的状态。
- (id)initWithImpl:(aurora::MacOSSurface::Impl *)impl_;
@end

// 无边框窗口仍需可成为 key/main window（NSWindow 默认对 borderless 返回 NO）。
@interface AuroraMacWindow : NSWindow
@end

namespace aurora {

/// @brief 全部 AppKit 状态（契约见 `MacOSSurface::Impl` 的公共头注释）。
///
/// public 而非 private：ObjC 视图与委托类需在类外以 `Impl *` 还原 user data 后转发，
/// 与 Wayland / X11 后端的 C listener 同一处置。所有权仍由 `impl_` 唯一持有。
struct MacOSSurface::Impl {
    NSWindow *window = nil;  ///< 原生窗口（Impl 唯一持有，releasedWhenClosed = NO）。
    NSView *view = nil;  ///< 内容视图（窗口持有；此处为非拥有观察指针）。
    id delegate = nil;  ///< 窗口委托（Impl 持有；窗口的 delegate 为 assign 语义）。
    NSWindow *owner_window = nil;  ///< 当前从属父窗口（非拥有；用于解除）。
    CGImageRef image = nullptr;  ///< 最近一帧的 CGImage（零拷贝引用 Painter 缓冲）。
    bool transparent = false;  ///< 透明窗口（CGImage 保留 alpha 通道）。
    bool should_close = false;  ///< 关闭请求标志（见 should_close()）。
    bool app_ready = false;  ///< NSApplication 已初始化（进程级一次）。
    bool active = false;  ///< 窗口是否前台激活（WindowState 推导输入）。
    bool marked = false;  ///< IME 是否有未上屏的 marked text。
    NSUInteger marked_len = 0;  ///< marked text 的 UTF-16 长度（markedRange 用）。
    float scale = 1.0F;  ///< 最近一次观测的 backingScaleFactor。
    ModifierKey modifiers = ModifierKey::None;  ///< 键盘路径修饰位缓存（鼠标/滚轮/拖放共用）。
    WindowMode last_mode = WindowMode::Normal;  ///< 最近一次上报的几何态（去重）。
    WindowState last_state = WindowState::Occluded;  ///< 最近一次上报的可见性态（去重）。
    ColorScheme last_scheme = ColorScheme::Unspecified;  ///< 最近一次上报的 OS 明暗档（去重）。
    int presented = 0;  ///< 成功上屏帧数。
    MacOSSurface *owner = nullptr;  ///< 回指（派发/通知的唯一出口）。
    Surface::EventHandler event_handler;  ///< 事件接收器。
    std::function<Rect()> caret_provider;  ///< IME 候选窗定位查询（窗口逻辑 dp）。
    /// @brief 系统拖放桥（契约与会话状态机见 `detail::MacOSDragBridge`），本 Impl 唯一持有。
    std::unique_ptr<detail::MacOSDragBridge> drag;
    /// @brief 最近文档 OS 投影桥（宿主引用守卫，见 `detail/macos_recent_documents.h`）。
    std::unique_ptr<detail::MacOSRecentDocumentsBridge> recent_docs;

    /// @brief 把事件交给宿主派发器（未注册时静默丢弃）。
    /// @param e 待派发事件（派发器可写 `is_handled`）。
    auto emit(Event &e) -> void {
        if (event_handler != nullptr) {
            event_handler(e);
        }
    }

    /// @brief 请求宿主同步补一帧（窗口几何/DPI 变化时调用；未接线时为空操作）。
    auto request_present() -> void {
        if (owner != nullptr && owner->present_request_) {
            owner->present_request_();
        }
    }

    /// @brief 观测并同步 backingScaleFactor；变化时上报 `notify_scale_change`。
    auto sync_scale() -> void {
        float s = 1.0F;
        if (window != nil) {
            s = static_cast<float>([window backingScaleFactor]);
        }
        if (s <= 0.0F) {
            s = 1.0F;
        }
        if (std::fabs(s - scale) > 0.001F) {
            scale = s;
            if (owner != nullptr) {
                owner->notify_scale_change(scale);
            }
        }
    }

    /// @brief 由 AppKit 当前状态推导几何态并在变化时上报。
    auto sync_mode() -> void {
        if (window == nil) {
            return;
        }
        const bool minimized = [window isMiniaturized];
        const bool fullscreen = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        const bool zoomed = !fullscreen && [window isZoomed];
        const WindowMode mode = compute_window_mode(minimized, zoomed, fullscreen);
        if (mode != last_mode) {
            last_mode = mode;
            if (owner != nullptr) {
                owner->notify_window_mode(mode);
            }
        }
    }

    /// @brief 由最小化/激活标志推导可见性态并在变化时上报。
    auto sync_state() -> void {
        if (window == nil) {
            return;
        }
        const WindowState state = compute_window_state([window isMiniaturized], active);
        if (state != last_state) {
            last_state = state;
            if (owner != nullptr) {
                owner->notify_window_state(state);
            }
        }
    }

    /// @brief 询问 AppKit 当前明暗档并在变化时上报 `notify_os_color_scheme_change`。
    ///
    /// 触发点：内容视图的 `viewDidChangeEffectiveAppearance`（用户切换系统明暗、应用外观
    /// 跟随系统变化时 AppKit 送达）。`Unspecified` 不参与去重也不上报——不知道不猜。
    auto sync_color_scheme() -> void {
        if (!app_ready) {
            return;
        }
        const ColorScheme scheme = aurora::detail::macos_os_color_scheme();
        if (scheme == ColorScheme::Unspecified || scheme == last_scheme) {
            return;
        }
        last_scheme = scheme;
        if (owner != nullptr) {
            owner->notify_os_color_scheme_change(scheme);
        }
    }

    /// @brief 安装系统拖放桥（窗口壳建好之后调用：登记与发起都以内容视图为落点）。
    ///
    /// 失败即本窗口不参与系统拖放，如实记日志降级、不谎报已有通路。
    auto install_drag() -> void {
        if (view == nil) {
            AURORA_LOG_INFO("window", "MacOSSurface: no content view; system drag not installed.");
            return;
        }
        drag = std::make_unique<detail::MacOSDragBridge>(
            (void *)view, detail::MacOSDragBridge::Hooks{.emit = [this](Event &e) -> void { emit(e); },
                                                         .scale_factor = [this]() -> float { return scale; },
                                                         .modifiers = [this]() -> ModifierKey { return modifiers; }});
        if (!drag->install()) {
            drag.reset();
            AURORA_LOG_WARN("window", "MacOSSurface: drag bridge install failed; system drag unavailable.");
        }
    }

    /// @brief 创建 NSWindow + 自定义 NSView，并按可见性策略入视野。
    /// @param w 初始内容区宽（逻辑 dp）。
    /// @param h 初始内容区高（逻辑 dp）。
    /// @param title UTF-8 标题。
    /// @param style 样式选项（装饰 / 置顶 / 透明 / 尺寸限制）。
    /// @param visibility 可见性策略。
    /// @return 创建成功 true；AppKit 初始化失败时 false。
    auto create_window(int w, int h, const std::string &title, const WindowStyleOptions &style,
                       WindowVisibility visibility) -> bool {
        @autoreleasepool {
            NSApplication *app = [NSApplication sharedApplication];
            if (app == nil) {
                AURORA_LOG_ERROR("macos_surface", "MacOSSurface: NSApplication unavailable.");
                return false;
            }
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            [app finishLaunching];
            app_ready = true;

            transparent = style.transparent;
            const bool frameless = style.frameless || style.decoration == DecorationPolicy::Frameless ||
                                   style.decoration == DecorationPolicy::Borderless;
            NSWindowStyleMask mask = 0;
            if (frameless) {
                mask = NSWindowStyleMaskBorderless;
            } else {
                mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
                if (style.resizable) {
                    mask |= NSWindowStyleMaskResizable;
                }
            }
            const int cw = std::max(1, w);
            const int ch = std::max(1, h);
            const NSRect content = NSMakeRect(0.0, 0.0, static_cast<CGFloat>(cw), static_cast<CGFloat>(ch));

            window =
                [[NSWindow alloc] initWithContentRect:content styleMask:mask backing:NSBackingStoreBuffered defer:NO];
            if (window == nil) {
                AURORA_LOG_ERROR("macos_surface", "MacOSSurface: NSWindow allocation failed.");
                return false;
            }
            [window setReleasedWhenClosed:NO];
            [window setRestorable:NO];
            [window setAcceptsMouseMovedEvents:YES];
            [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
            [window setLevel:style.always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel];

            const char *utf8 = title.c_str();
            NSString *ns_title = [NSString stringWithUTF8String:utf8];
            [window setTitle:ns_title != nil ? ns_title : @""];

            if (transparent) {
                [window setOpaque:NO];
                [window setBackgroundColor:[NSColor clearColor]];
            } else {
                [window setOpaque:YES];
            }

            // 内容区尺寸限制：0 表示该方向不限；不可缩放时锁死为初始尺寸。
            if (!style.resizable) {
                [window setContentMinSize:NSMakeSize(static_cast<CGFloat>(cw), static_cast<CGFloat>(ch))];
                [window setContentMaxSize:NSMakeSize(static_cast<CGFloat>(cw), static_cast<CGFloat>(ch))];
            } else {
                if (style.min_size.width > 0.0F && style.min_size.height > 0.0F) {
                    [window setContentMinSize:NSMakeSize(static_cast<CGFloat>(style.min_size.width),
                                                         static_cast<CGFloat>(style.min_size.height))];
                }
                if (style.max_size.width > 0.0F && style.max_size.height > 0.0F) {
                    [window setContentMaxSize:NSMakeSize(static_cast<CGFloat>(style.max_size.width),
                                                         static_cast<CGFloat>(style.max_size.height))];
                }
            }

            view = [[AuroraMacSurfaceView alloc] initWithFrame:content impl:this];
            if (view == nil) {
                AURORA_LOG_ERROR("macos_surface", "MacOSSurface: content view allocation failed.");
                [window release];
                window = nil;
                return false;
            }
            [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [window setContentView:view];
            // MRC：窗口已持有内容视图（setContentView: 为 retain 语义），释放本地 alloc 引用。
            // Impl::view 是随窗口存活的非拥有观察指针，之后的 makeFirstResponder / 拖放登记
            // 与 destroy_window 内的 setContentView:nil 都以窗口的持有为准。
            [view release];
            [window makeFirstResponder:view];

            delegate = [[AuroraMacWindowDelegate alloc] initWithImpl:this];
            [window setDelegate:delegate];

            [window center];
            scale = static_cast<float>([window backingScaleFactor]);
            if (scale <= 0.0F) {
                scale = 1.0F;
            }

            switch (visibility) {
                case WindowVisibility::Normal:
                    activate_app();
                    [window makeKeyAndOrderFront:nil];
                    [window makeFirstResponder:view];
                    active = true;
                    break;
                case WindowVisibility::NoActivate:
                    [window orderFrontRegardless];
                    break;
                case WindowVisibility::Hidden:
                    break;
            }

            install_drag();
            recent_docs = std::make_unique<detail::MacOSRecentDocumentsBridge>();
            // 原生菜单栏是应用级对象（NSApp.mainMenu），登记一次即随进程存活；失败无路径可报，
            // 只保证 NativeMenuBar::install 在首个窗口建出后即可用。
            static_cast<void>(detail::macos_menu_bar_ensure_registered());
            last_mode = WindowMode::Normal;
            last_state = compute_window_state(false, active);
            // 记录初始明暗档（不上报）：宿主在 WindowHost 构造期自行调用 os_color_scheme()
            // 取快照，这里只建立「之后的变化」的比较基准。
            last_scheme = aurora::detail::macos_os_color_scheme();
        }
        return true;
    }

    /// @brief 激活本应用（兼容新旧 SDK：`activate` 仅在 macOS 14+ 可用）。
    static auto activate_app() -> void {
        if ([NSApp respondsToSelector:@selector(activate)]) {
            [NSApp performSelector:@selector(activate)];
        } else {
// 旧 SDK 的唯一入口；新版 SDK 上该调用虽弃用但仍在运行时有效。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        }
    }

    /// @brief 收摊：先注销系统拖放桥（在途会话按取消兑现），再释放视图/委托/窗口/图像。
    void destroy_window() {
        drag.reset();
        recent_docs.reset();
        if (window != nil) {
            [window setDelegate:nil];
            [window orderOut:nil];
            [window setContentView:nil];
        }
        if (image != nullptr) {
            CGImageRelease(image);
            image = nullptr;
        }
        if (delegate != nil) {
            [delegate release];
            delegate = nil;
        }
        if (window != nil) {
            [window release];
            window = nil;
        }
        view = nil;
        owner_window = nil;
    }
};

}  // namespace aurora

namespace aurora {

// =============================================================================
// 静态辅助
// =============================================================================

namespace {

/// @brief 主屏高度（Cocoa 全局屏幕坐标以主屏左下角为原点，y 向上）。
/// @return 主屏 frame 高度（point）；无屏幕时 0。
auto primary_screen_height_points() -> CGFloat {
    NSArray<NSScreen *> *screens = [NSScreen screens];
    NSScreen *primary = [screens count] > 0 ? [screens objectAtIndex:0] : [NSScreen mainScreen];
    return primary != nil ? primary.frame.size.height : static_cast<CGFloat>(0);
}

/// @brief NSString → UTF-8（非法 UTF-8 时返回空串）。
/// @param s 源字符串（可为 nil）。
/// @return UTF-8 字节串。
auto ns_to_utf8(NSString *s) -> std::string {
    if (s == nil) {
        return {};
    }
    const char *utf8 = [s UTF8String];
    return utf8 != nullptr ? std::string(utf8) : std::string{};
}

/// @brief NSString → UTF-16（NSTextInputClient 的索引口径）。
/// @param s 源字符串（可为 nil）。
/// @return UTF-16 串。
auto ns_to_utf16(NSString *s) -> std::u16string {
    if (s == nil) {
        return {};
    }
    const NSUInteger len = [s length];
    std::u16string out(len, u'\0');
    if (len > 0) {
        static_assert(sizeof(unichar) == sizeof(char16_t), "unichar must be 16-bit");
        [s getCharacters:reinterpret_cast<unichar *>(out.data()) range:NSMakeRange(0, len)];
    }
    return out;
}

/// @brief 把当前鼠标事件的按键编号折算为框架按键。
/// @param event 鼠标事件。
/// @return 左/右/中；其余编号返回 std::nullopt。
auto button_for_event(NSEvent *event) -> std::optional<MouseButton> {
    switch ([event buttonNumber]) {
        case 0:
            return MouseButton::Left;
        case 1:
            return MouseButton::Right;
        case 2:
            return MouseButton::Middle;
        default:
            return std::nullopt;
    }
}

/// @brief 构造并派发一条鼠标事件。
/// @param impl 目标状态。
/// @param event AppKit 事件（取窗口坐标与修饰位）。
/// @param action 指针动作。
auto dispatch_mouse(aurora::MacOSSurface::Impl *impl, NSEvent *event, MouseAction action) -> void {
    if (impl == nullptr || impl->view == nil) {
        return;
    }
    const auto button = button_for_event(event);
    if (!button.has_value()) {
        return;
    }
    const NSPoint p = [impl->view convertPoint:[event locationInWindow] fromView:nil];
    MouseEvent e;
    e.action = action;
    e.button = *button;
    e.position = Point{.x = static_cast<float>(p.x), .y = static_cast<float>(p.y)};
    e.modifiers = aurora::detail::macos_modifiers_from_flags(static_cast<std::uint64_t>([event modifierFlags]));
    impl->modifiers = e.modifiers;
    impl->emit(e);
}

}  // namespace

}  // namespace aurora

// =============================================================================
// ObjC 实现
// =============================================================================

@implementation AuroraMacWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

@implementation AuroraMacSurfaceView

- (id)initWithFrame:(NSRect)frame impl:(aurora::MacOSSurface::Impl *)impl_ {
    self = [super initWithFrame:frame];
    if (self != nil) {
        impl = impl_;
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (BOOL)wantsUpdateLayer {
    return NO;
}

// 系统明暗 / 应用外观变化：把新的档位上报给宿主（明暗三级优先链的 OS 层）。
- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    if (impl != nullptr) {
        impl->sync_color_scheme();
    }
}

// ---- 绘制 ----

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    if (gc == nil || impl == nullptr) {
        return;
    }
    CGContextRef ctx = [gc CGContext];
    if (ctx == nullptr) {
        return;
    }
    const NSRect bounds = self.bounds;
    if (impl->image != nullptr) {
        // 视图为翻转坐标：局部翻回 y-up 后按物理像素 1:1 绘制（Retina 下不缩放）。
        CGContextSaveGState(ctx);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextTranslateCTM(ctx, 0.0, bounds.size.height);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextDrawImage(ctx, CGRectMake(0.0, 0.0, bounds.size.width, bounds.size.height), impl->image);
        CGContextRestoreGState(ctx);
        return;
    }
    // 尚未出过帧（或缓冲换址后的瞬态）：铺与 begin_frame 一致的底色；
    // 透明窗口留空，避免遮住桌面。
    if (!impl->transparent) {
        CGContextSetRGBFillColor(ctx, 245.0 / 255.0, 245.0 / 255.0, 247.0 / 255.0, 1.0);
        CGContextFillRect(ctx, CGRectMake(0.0, 0.0, bounds.size.width, bounds.size.height));
    }
}

// ---- 鼠标 ----

- (void)mouseDown:(NSEvent *)event {
    if (impl != nullptr && impl->drag) {
        aurora::detail::MacOSDragBridge::note_initiator(impl->drag.get());
        impl->drag->note_press_event((void *)event);
    }
    [self.window makeFirstResponder:self];
    aurora::dispatch_mouse(impl, event, MouseAction::Press);
}

- (void)mouseUp:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Release);
}

- (void)mouseMoved:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Move);
}

- (void)mouseDragged:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Move);
}

- (void)rightMouseDown:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Press);
}

- (void)rightMouseUp:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Release);
}

- (void)rightMouseDragged:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Move);
}

- (void)otherMouseDown:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Press);
}

- (void)otherMouseUp:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Release);
}

- (void)otherMouseDragged:(NSEvent *)event {
    aurora::dispatch_mouse(impl, event, MouseAction::Move);
}

- (void)scrollWheel:(NSEvent *)event {
    if (impl == nullptr) {
        return;
    }
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    CGFloat dx = [event scrollingDeltaX];
    CGFloat dy = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas]) {
        // 精确设备（触控板/妙控鼠标）的增量以像素计：折算到与滚轮同量级的「行」。
        // 与 GLFW 的 Cocoa 后端同系数（0.1），使同一台机器上两个后端的滚动手感一致。
        dx *= 0.1;
        dy *= 0.1;
    }
    ScrollEvent e;
    e.position = aurora::Point{.x = static_cast<float>(p.x), .y = static_cast<float>(p.y)};
    e.delta_x = static_cast<float>(dx);
    e.delta_y = static_cast<float>(dy);
    e.modifiers = aurora::detail::macos_modifiers_from_flags(static_cast<std::uint64_t>([event modifierFlags]));
    impl->modifiers = e.modifiers;
    impl->emit(e);
}

// ---- 键盘 ----

- (void)keyDown:(NSEvent *)event {
    if (impl == nullptr) {
        return;
    }
    const std::uint64_t flags = static_cast<std::uint64_t>([event modifierFlags]);
    impl->modifiers = aurora::detail::macos_modifiers_from_flags(flags);
    KeyEvent e;
    e.key = static_cast<int>(aurora::detail::macos_keycode_to_keycode([event keyCode]));
    e.action = KeyAction::Down;
    e.modifiers = impl->modifiers;
    impl->emit(e);
    // Command 组合是快捷键而非文本：不进入输入法/字符解释路径（菜单/快捷键由宿主处理）。
    if ((flags & aurora::detail::AURORA_MACOS_MOD_COMMAND) == 0U) {
        [self interpretKeyEvents:@[ event ]];
    }
}

- (void)keyUp:(NSEvent *)event {
    if (impl == nullptr) {
        return;
    }
    const std::uint64_t flags = static_cast<std::uint64_t>([event modifierFlags]);
    impl->modifiers = aurora::detail::macos_modifiers_from_flags(flags);
    KeyEvent e;
    e.key = static_cast<int>(aurora::detail::macos_keycode_to_keycode([event keyCode]));
    e.action = KeyAction::Up;
    e.modifiers = impl->modifiers;
    impl->emit(e);
}

- (void)flagsChanged:(NSEvent *)event {
    if (impl == nullptr) {
        return;
    }
    const std::uint64_t flags = static_cast<std::uint64_t>([event modifierFlags]);
    impl->modifiers = aurora::detail::macos_modifiers_from_flags(flags);
    const KeyCode key = aurora::detail::macos_keycode_to_keycode([event keyCode]);
    ModifierKey bit = ModifierKey::None;
    switch (key) {
        case KeyCode::Shift:
            bit = ModifierKey::Shift;
            break;
        case KeyCode::Control:
            bit = ModifierKey::Control;
            break;
        case KeyCode::Alt:
            bit = ModifierKey::Alt;
            break;
        case KeyCode::Meta:
            bit = ModifierKey::Meta;
            break;
        default:
            return;  // CapsLock / Fn 等：框架不建模为修饰位，如实丢弃
    }
    const auto mask = static_cast<std::uint8_t>(impl->modifiers) & static_cast<std::uint8_t>(bit);
    KeyEvent e;
    e.key = static_cast<int>(key);
    e.action = mask != 0U ? KeyAction::Down : KeyAction::Up;
    e.modifiers = impl->modifiers;
    impl->emit(e);
}

// ---- NSTextInputClient（文本输入与 IME）----

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;  // 框架侧文本状态由控件维护；替换范围交由控件的选区语义处理
    if (impl == nullptr) {
        return;
    }
    NSString *text =
        [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    const std::string utf8 = aurora::ns_to_utf8(text);
    if (utf8.empty()) {
        return;
    }
    if (impl->marked) {
        // 组合期内上屏：以组合事件的 committed 通道落字并结束组合（与 Win32/Wayland 同口径）。
        impl->marked = false;
        impl->marked_len = 0;
        TextCompositionEvent e;
        e.committed = utf8;
        impl->emit(e);
        return;
    }
    TextInputEvent e;
    e.text = utf8;
    impl->emit(e);
}

- (void)insertText:(id)string {
    [self insertText:string replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (impl == nullptr) {
        return;
    }
    NSString *text =
        [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    const std::u16string u16 = aurora::ns_to_utf16(text);
    if (u16.empty()) {
        impl->marked = false;
        impl->marked_len = 0;
        TextCompositionEvent e;  // 全默认：preedit 空 = 组合结束/取消
        impl->emit(e);
        return;
    }
    // 已选区间标注为「待转换段」：折算层把连续目标段映射到 sel_start/sel_end 码点区间。
    std::vector<std::uint8_t> attrs(u16.size(), 0U);
    const NSUInteger sel_begin = selectedRange.location == NSNotFound ? 0 : selectedRange.location;
    const NSUInteger sel_end = selectedRange.location == NSNotFound ? 0 : selectedRange.location + selectedRange.length;
    const NSUInteger clamped_begin = std::min<NSUInteger>(sel_begin, u16.size());
    const NSUInteger clamped_end = std::min<NSUInteger>(sel_end, u16.size());
    for (NSUInteger i = clamped_begin; i < clamped_end; ++i) {
        attrs[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(aurora::ime::Attr::TargetNotConverted);
    }
    TextCompositionEvent e = aurora::ime::make_preedit_state(u16, clamped_begin, attrs);
    impl->marked = true;
    impl->marked_len = u16.size();
    impl->emit(e);
}

- (void)unmarkText {
    if (impl == nullptr || !impl->marked) {
        return;
    }
    impl->marked = false;
    impl->marked_len = 0;
    TextCompositionEvent e;  // 组合被取消：preedit 置空，控件清理显示态
    impl->emit(e);
}

- (BOOL)hasMarkedText {
    return impl != nullptr && impl->marked;
}

- (NSRange)markedRange {
    if (impl == nullptr || !impl->marked) {
        return NSMakeRange(NSNotFound, 0);
    }
    return NSMakeRange(0, impl->marked_len);
}

- (NSRange)selectedRange {
    // 视图本身不持有文本（文本状态在控件侧），无有效选区。
    return NSMakeRange(NSNotFound, 0);
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
    return nil;
}

- (NSArray<NSString *> *)validAttributesForMarkedText {
    return @[];
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;  // 无原生文本视图：命令已由 KeyEvent 通道交给控件，此处静默防系统提示音
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    if (actualRange != nullptr) {
        *actualRange = NSMakeRange(NSNotFound, 0);
    }
    if (impl == nullptr) {
        return NSZeroRect;
    }
    NSRect logical = NSZeroRect;
    if (impl->caret_provider) {
        const aurora::Rect caret = impl->caret_provider();
        const CGFloat w = caret.size.width > 0.0F ? static_cast<CGFloat>(caret.size.width) : 1.0;
        const CGFloat h = caret.size.height > 0.0F ? static_cast<CGFloat>(caret.size.height) : 1.0;
        // 逻辑 dp（y 向下）→ 视图坐标（翻转视图同为 y 向下），再换到窗口/屏幕坐标。
        logical = NSMakeRect(static_cast<CGFloat>(caret.origin.x), static_cast<CGFloat>(caret.origin.y), w, h);
    }
    const NSRect in_window = [self convertRect:logical toView:nil];
    return self.window != nil ? [self.window convertRectToScreen:in_window] : NSZeroRect;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

// ---- NSDraggingDestination（转发到拖放桥）----

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if (impl == nullptr || !impl->drag) {
        return NSDragOperationNone;
    }
    return (NSDragOperation)impl->drag->on_dragging_entered((void *)sender);
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if (impl == nullptr || !impl->drag) {
        return NSDragOperationNone;
    }
    return (NSDragOperation)impl->drag->on_dragging_updated((void *)sender);
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    if (impl != nullptr && impl->drag) {
        impl->drag->on_dragging_exited();
    }
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    if (impl == nullptr || !impl->drag) {
        return NO;
    }
    return impl->drag->on_prepare_for_drag_operation((void *)sender) ? YES : NO;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (impl == nullptr || !impl->drag) {
        return NO;
    }
    impl->drag->on_perform_drag_operation((void *)sender);
    return YES;
}

- (BOOL)wantsPeriodicDraggingUpdates {
    return NO;
}

@end

@implementation AuroraMacWindowDelegate

- (id)initWithImpl:(aurora::MacOSSurface::Impl *)impl_ {
    self = [super init];
    if (self != nil) {
        impl = impl_;
    }
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if (impl == nullptr) {
        return YES;
    }
    // 只登记请求，不关闭原生窗口：回收时机由框架的关闭闸门决定
    // （windowShouldClose: 返回 NO 才不会让 AppKit 抢先 orderOut/release）。
    impl->should_close = true;
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->active = true;
    impl->modifiers = aurora::detail::macos_modifiers_from_flags(static_cast<std::uint64_t>([NSEvent modifierFlags]));
    impl->sync_state();
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->active = false;
    impl->modifiers = aurora::ModifierKey::None;  // 失焦后修饰键事件不再送达，缓存作废
    impl->sync_state();
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_mode();
    impl->sync_state();
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_mode();
    impl->sync_state();
    impl->request_present();
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_scale();
    impl->sync_mode();
    impl->request_present();
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_scale();
    impl->sync_mode();
    impl->request_present();
}

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_mode();
    // 几何变化同步补一帧：窗口在合成前已持有新尺寸内容，避免最大化/缩放时白闪。
    impl->request_present();
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_scale();
    impl->request_present();
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    if (impl == nullptr) {
        return;
    }
    impl->sync_scale();
    impl->request_present();
}

@end

namespace aurora {

// =============================================================================
// MacOSSurface
// =============================================================================

MacOSSurface::MacOSSurface(int w, int h, const std::string &title, const WindowStyleOptions &style,
                           WindowVisibility visibility)
    : impl_(std::make_unique<Impl>()), size_{.width = static_cast<float>(w), .height = static_cast<float>(h)} {
    impl_->owner = this;
    if (!impl_->create_window(w, h, title, style, visibility)) {
        AURORA_LOG_WARN("macos_surface", "MacOSSurface: native window creation failed.");
    }
}

MacOSSurface::~MacOSSurface() {
    @autoreleasepool {
        if (impl_) {
            impl_->destroy_window();
        }
    }
}

auto MacOSSurface::begin_frame(int w, int h) -> Result<bool> {
    if (!impl_) {
        return Result<bool>{make_error(ErrorCode::SurfaceLost, "macos begin_frame: surface state missing")};
    }
    Impl &d = *impl_;
    d.sync_scale();
    painter_.set_scale(d.scale);

    // 尺寸以原生内容视图的真实 bounds 为准（窗口 resize 通知后调用方传参可能滞后一帧）；
    // 视图缺失时回退到入参。
    int lw = w > 0 ? w : 1;
    int lh = h > 0 ? h : 1;
    if (d.view != nil) {
        const NSSize bs = [d.view bounds].size;
        if (bs.width >= 1.0 && bs.height >= 1.0) {
            lw = static_cast<int>(std::lround(static_cast<double>(bs.width)));
            lh = static_cast<int>(std::lround(static_cast<double>(bs.height)));
        }
    }
    if (lw <= 0) {
        lw = 1;
    }
    if (lh <= 0) {
        lh = 1;
    }
    const int phys_w = static_cast<int>(std::lround(static_cast<float>(lw) * d.scale));
    const int phys_h = static_cast<int>(std::lround(static_cast<float>(lh) * d.scale));
    // Painter 换址前必须释放零拷贝 CGImage，否则视图重绘会读已释放缓冲。
    if (d.image != nullptr && (phys_w != painter_.width() || phys_h != painter_.height())) {
        CGImageRelease(d.image);
        d.image = nullptr;
    }
    if (phys_w != painter_.width() || phys_h != painter_.height()) {
        painter_.begin(lw, lh);
    }
    painter_.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                            .size = Size{.width = static_cast<float>(lw), .height = static_cast<float>(lh)}},
                       Color{245, 245, 247, 255});
    size_ = Size{.width = static_cast<float>(lw), .height = static_cast<float>(lh)};
    return Result<bool>{true};
}

auto MacOSSurface::painter() -> Painter & { return painter_; }

auto MacOSSurface::present() -> Result<bool> {
    if (!impl_ || impl_->window == nil || impl_->view == nil) {
        return Result<bool>{make_error(ErrorCode::SurfaceLost, "macos present: native window unavailable")};
    }
    Impl &d = *impl_;
    const int pw = painter_.width();
    const int ph = painter_.height();
    const std::uint8_t *pixels = painter_.data();
    if (pw <= 0 || ph <= 0 || pixels == nullptr) {
        return Result<bool>{make_error(ErrorCode::SurfaceLost, "macos present: framebuffer unavailable")};
    }
    if (d.image != nullptr) {
        CGImageRelease(d.image);
        d.image = nullptr;
    }
    // 零拷贝包装：CGImage 直接引用 Painter 缓冲（下一次 begin_frame 换址前先释放本图像）。
    const std::size_t byte_count = static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 4U;
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, const_cast<std::uint8_t *>(pixels), byte_count, nullptr);
    if (provider == nullptr) {
        return Result<bool>{
            make_error(ErrorCode::GeneralUnknown, "macos present: CGDataProviderCreateWithData failed")};
    }
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    // 不透明窗口忽略 alpha（Painter 输出为直通 alpha，半透明边缘的有效色已在底色上合成）；
    // 透明窗口保留直通 alpha（kCGImageAlphaLast），避免误当预乘造成边缘发亮。
    const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big) |
                                     (d.transparent ? static_cast<CGBitmapInfo>(kCGImageAlphaLast)
                                                    : static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast));
    CGImageRef image =
        CGImageCreate(static_cast<size_t>(pw), static_cast<size_t>(ph), 8, 32, static_cast<size_t>(pw) * 4U,
                      color_space, bitmap_info, provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color_space);
    CGDataProviderRelease(provider);
    if (image == nullptr) {
        return Result<bool>{make_error(ErrorCode::GeneralUnknown, "macos present: CGImageCreate failed")};
    }
    d.image = image;
    [d.view setNeedsDisplay:YES];
    [d.view displayIfNeeded];
    ++d.presented;
    return Result<bool>{true};
}

auto MacOSSurface::size() const -> Size {
    if (impl_ && impl_->view != nil) {
        const NSSize bs = [impl_->view bounds].size;
        if (bs.width > 0.0 && bs.height > 0.0) {
            return Size{.width = static_cast<float>(bs.width), .height = static_cast<float>(bs.height)};
        }
    }
    return size_;
}

auto MacOSSurface::scale_factor() const -> float {
    if (impl_ && impl_->window != nil) {
        const float s = static_cast<float>([impl_->window backingScaleFactor]);
        return s > 0.0F ? s : 1.0F;
    }
    return 1.0F;
}

auto MacOSSurface::should_close() const -> bool {
    // 窗口壳不存在（创建失败/已销毁）时视同「已请求关闭」，宿主据此回收。
    return (impl_ == nullptr || impl_->window == nil) ? true : impl_->should_close;
}

auto MacOSSurface::cancel_close() -> void {
    // 窗口已被 OS 销毁的路径必须拒绝清位（否则造出永远收不掉的僵尸宿主）。
    if (impl_ != nullptr && impl_->window != nil) {
        impl_->should_close = false;
    }
}

auto MacOSSurface::poll_platform_events() -> void {
    if (!impl_ || !impl_->app_ready) {
        return;
    }
    @autoreleasepool {
        if (impl_->drag) {
            impl_->drag->tick();
        }
        for (;;) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event == nil) {
                break;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}

auto MacOSSurface::wait_events(double timeout_ms) -> void {
    if (!impl_ || !impl_->app_ready || timeout_ms == 0.0) {
        return;
    }
    // 无限等待按 1000ms 分段兜底：即便跨线程唤醒通道失效，主循环也不会永久睡死。
    const double capped = timeout_ms < 0.0 ? 1000.0 : std::min(timeout_ms, 1000.0);
    @autoreleasepool {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:capped / 1000.0];
        NSEvent *event =
            [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event != nil) {
            [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
}

auto MacOSSurface::request_wake() -> void {
    if (!impl_ || !impl_->app_ready) {
        return;
    }
    @autoreleasepool {
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        if (event != nil) {
            [NSApp postEvent:event atStart:YES];
        }
    }
}

auto MacOSSurface::pumps_thread_queue() const -> bool { return true; }

auto MacOSSurface::waits_thread_queue() const -> bool { return true; }

auto MacOSSurface::set_event_handler(const EventHandler &h) -> void {
    if (impl_ != nullptr) {
        impl_->event_handler = h;
    }
}

auto MacOSSurface::set_title(const std::string &title) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    NSString *ns_title = [NSString stringWithUTF8String:title.c_str()];
    [impl_->window setTitle:ns_title != nil ? ns_title : @""];
}

auto MacOSSurface::set_size(Size s) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    const CGFloat w = static_cast<CGFloat>(std::max(1.0F, s.width));
    const CGFloat h = static_cast<CGFloat>(std::max(1.0F, s.height));
    [impl_->window setContentSize:NSMakeSize(w, h)];
    // 同步逻辑尺寸缓存：几何恢复在首帧之前下发，若等 resize 通知才更新，首帧会按旧尺寸布局。
    size_ = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
}

auto MacOSSurface::set_position(Point p) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    const float scale = scale_factor();
    const CGFloat pt_x = static_cast<CGFloat>(p.x) / static_cast<CGFloat>(scale);
    const CGFloat top = static_cast<CGFloat>(p.y) / static_cast<CGFloat>(scale);
    const NSRect frame = [impl_->window frame];
    // 物理像素（左上角原点，y 向下）→ Cocoa 全局坐标（主屏左下角原点，y 向上）。
    const CGFloat origin_y = primary_screen_height_points() - top - frame.size.height;
    [impl_->window setFrameOrigin:NSMakePoint(pt_x, origin_y)];
}

auto MacOSSurface::position() const -> Point {
    if (!impl_ || impl_->window == nil) {
        return Point{.x = 0.0F, .y = 0.0F};
    }
    const NSRect frame = [impl_->window frame];
    const CGFloat scale = static_cast<CGFloat>(scale_factor());
    const CGFloat top = primary_screen_height_points() - (frame.origin.y + frame.size.height);
    return Point{.x = static_cast<float>(frame.origin.x * scale), .y = static_cast<float>(top * scale)};
}

auto MacOSSurface::display_id() const -> int {
    if (!impl_ || impl_->window == nil) {
        return -1;
    }
    NSScreen *screen = [impl_->window screen];
    if (screen == nil) {
        return -1;
    }
    NSDictionary *description = [screen deviceDescription];
    NSNumber *number = description != nil ? [description objectForKey:@"NSScreenNumber"] : nil;
    return number != nil ? static_cast<int>([number unsignedIntValue]) : -1;
}

auto MacOSSurface::minimize() -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    if (![impl_->window isMiniaturized]) {
        [impl_->window miniaturize:nil];
    }
}

auto MacOSSurface::toggle_maximize() -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    if (([impl_->window styleMask] & NSWindowStyleMaskFullScreen) != 0) {
        [impl_->window toggleFullScreen:nil];  // 全屏态下「最大化」语义退回普通/缩放态
        return;
    }
    [impl_->window zoom:nil];
    impl_->sync_mode();
    impl_->request_present();
}

auto MacOSSurface::set_fullscreen(bool on) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    const bool current = ([impl_->window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    if (current != on) {
        [impl_->window toggleFullScreen:nil];
    }
}

auto MacOSSurface::set_window_mode(WindowMode m) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    NSWindow *window = impl_->window;
    const bool fullscreen = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    const bool minimized = [window isMiniaturized];
    const bool zoomed = !fullscreen && [window isZoomed];

    switch (m) {
        case WindowMode::FullScreen:
            if (minimized) {
                [window deminiaturize:nil];
            }
            if (!fullscreen) {
                [window toggleFullScreen:nil];
            }
            break;
        case WindowMode::Minimized:
            if (fullscreen) {
                [window toggleFullScreen:nil];
            }
            if (!minimized) {
                [window miniaturize:nil];
            }
            break;
        case WindowMode::Maximized:
            if (minimized) {
                [window deminiaturize:nil];
            }
            if (fullscreen) {
                [window toggleFullScreen:nil];
            } else if (!zoomed) {
                [window zoom:nil];
            }
            break;
        case WindowMode::Normal:
            if (minimized) {
                [window deminiaturize:nil];
            }
            if (fullscreen) {
                [window toggleFullScreen:nil];
            }
            if (zoomed) {
                [window zoom:nil];
            }
            break;
    }
    // 目标态语义：立即上报目标档；AppKit 的异步过渡完成后由委托再按真实态校正。
    if (m != impl_->last_mode) {
        impl_->last_mode = m;
        notify_window_mode(m);
    }
    impl_->request_present();
}

auto MacOSSurface::close() -> void {
    if (impl_ != nullptr) {
        impl_->should_close = true;  // 只置位，窗口回收由宿主完成
    }
}

auto MacOSSurface::raise() -> void {
    if (impl_ && impl_->window != nil) {
        [impl_->window orderFront:nil];
    }
}

auto MacOSSurface::focus_window() -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    Impl::activate_app();
    [impl_->window makeKeyAndOrderFront:nil];
    if (impl_->view != nil) {
        [impl_->window makeFirstResponder:impl_->view];
    }
}

auto MacOSSurface::set_owner(const Surface *owner_surface) -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    NSWindow *new_owner = nullptr;
    if (owner_surface != nullptr) {
        new_owner = (NSWindow *)owner_surface->native_handle();
    }
    if (impl_->owner_window != nil) {
        [(NSWindow *)impl_->owner_window removeChildWindow:impl_->window];
        impl_->owner_window = nil;
    }
    if (new_owner != nil && new_owner != impl_->window) {
        [new_owner addChildWindow:impl_->window ordered:NSWindowAbove];
        impl_->owner_window = new_owner;
    }
}

auto MacOSSurface::set_enabled(bool on) -> void {
    if (impl_ && impl_->window != nil) {
        [impl_->window setIgnoresMouseEvents:on ? NO : YES];
    }
}

auto MacOSSurface::begin_window_move() -> void {
    if (!impl_ || impl_->window == nil) {
        return;
    }
    NSEvent *event = [NSApp currentEvent];
    if (event == nil) {
        return;
    }
    switch ([event type]) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseDragged:
            [impl_->window performWindowDragWithEvent:event];
            break;
        default:
            break;  // 不在鼠标按下/拖拽派发栈内：performWindowDrag 会拒绝，直接忽略
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

auto MacOSSurface::set_present_request(PresentRequest h) -> void { present_request_ = std::move(h); }

auto MacOSSurface::data() const -> const std::uint8_t * {
    return painter_.data() != nullptr ? painter_.data() : nullptr;
}

auto MacOSSurface::framebuffer_size() const -> Size {
    if (painter_.width() > 0 && painter_.height() > 0) {
        return Size{.width = static_cast<float>(painter_.width()), .height = static_cast<float>(painter_.height())};
    }
    const float s = scale_factor();
    return Size{.width = size_.width * s, .height = size_.height * s};
}

auto MacOSSurface::native_handle() const -> void * {
    return (impl_ && impl_->window != nil) ? (void *)impl_->window : nullptr;
}

auto MacOSSurface::frame_count() const -> int { return impl_ ? impl_->presented : 0; }

auto MacOSSurface::clear_color() const -> Color { return Color{245, 245, 247, 255}; }

auto MacOSSurface::set_composition_caret_provider(std::function<Rect()> provider) -> void {
    if (impl_ != nullptr) {
        impl_->caret_provider = std::move(provider);
    }
}

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
