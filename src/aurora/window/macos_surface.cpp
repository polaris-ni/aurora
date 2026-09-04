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
// - 关闭语义：windowShouldClose: → m_should_close = true。
//
// 本文件为骨架，全部方法在 macOS 环境实写后替换 stub。

#include "aurora/core/platform.h"
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#include "aurora/core/log.h"
#include "aurora/window/macos_surface.h"

// AppKit 头需在 .cpp 内引入，避免公共头污染消费者
#import <AppKit/AppKit.h>

namespace aurora {

struct MacOSSurface::Impl {
    NSWindow *window = nullptr;
    NSView *view = nullptr;  ///< 自定义 NSView 子类实例（覆盖 drawRect:）
    bool should_close = false;
    float scale = 1.0F;
    Surface::EventHandler event_handler;

    /// @brief 创建 NSWindow + 自定义 NSView。
    bool create_window(int w, int h, const std::string &title) {
        // TODO(macOS): 实写 NSWindow 创建逻辑
        // 1. NSRect frame = NSMakeRect(0, 0, w, h)
        // 2. window = [[NSWindow alloc] initWithContentRect:frame styleMask:... backing:... defer:NO]
        // 3. [window setTitle:[NSString stringWithUTF8String:title.c_str()]]
        // 4. 创建自定义 NSView 子类（覆盖 drawRect:/acceptsFirstResponder/mouseDown: 等）
        // 5. [window setContentView:view]
        // 6. [window makeKeyAndOrderFront:nil]
        // 7. scale = [window backingScaleFactor]
        (void)w;
        (void)h;
        (void)title;
        AURORA_LOG_INFO("macos_surface", "create_window stub (implement for macOS)");
        return true;
    }

    void destroy_window() {
        // TODO(macOS): [window close]; window = nil; view = nil;
    }
};

MacOSSurface::MacOSSurface(int w, int h, const std::string &title)
    : impl_(std::make_unique<Impl>()), size_{static_cast<float>(w), static_cast<float>(h)} {
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
    size_ = Size{static_cast<float>(w), static_cast<float>(h)};
    // 浅色底色（与 Win32/X11 一致）
    painter_.fill_rect(Rect{Point{0.0F, 0.0F}, Size{static_cast<float>(w), static_cast<float>(h)}},
                        Color{245, 245, 247, 255});
    return true;
}

auto MacOSSurface::painter() -> Painter & { return painter_; }

auto MacOSSurface::present() -> Result<bool> {
    // TODO(macOS): 实写上屏
    // 1. 从 m_painter.data() 获取 RGBA 帧缓冲
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

}  // namespace aurora

#endif  // AURORA_BACKEND_MACOS / AURORA_PLATFORM_MACOS
