// display_macos.cpp — macOS 显示器枚举（NSScreen）。
//
// 与 `display_win32.cpp` 同口径：`Display::bounds` / `work_area` 为**物理像素**、原点在主屏
// 左上角、y 向下；`scale_factor` 为 `backingScaleFactor`。`Display::id` 取
// `NSScreenNumber`（`CGDirectDisplayID`），与 `MacOSSurface::display_id()` 同源，
// `move_window_to_display()` 因此能按 id 找回同一块屏。
//
// Cocoa 的全局屏幕坐标是「主屏左下角原点、y 向上、point 单位」：本文件是唯一的折算点，
// 以主屏高度做 y 翻转、再乘 backingScaleFactor 落到物理像素。NSScreen 的 `visibleFrame`
// 已扣除菜单栏与 Dock，直接作为 `work_area`。
//
// 门控：macOS 平台且 macOS 后端（无原生窗口壳时 `move_window_to_display` 没有落点，
// 枚举本身也无消费方；`display_win32.cpp` 的通用回退在该组合下不参与编译）。

#include "aurora/app/display.h"
#include "aurora/core/platform.h"  // NOLINT
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include <utility>  // std::cmp_equal（有符号 display_id 与无符号 NSScreenNumber 的比较）

namespace aurora::app {

namespace {

/// @brief 默认屏回退（无 GUI 会话 / 无显示器）：与其它平台的合成默认屏同形。
auto default_display() -> Display {
    Display d;
    d.id = -1;
    d.name = "default";
    d.bounds = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 1920.0F, .height = 1080.0F}};
    d.work_area = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 1920.0F, .height = 1080.0F}};
    d.scale_factor = 1.0F;
    d.is_primary = true;
    return d;
}

/// @brief NSScreen → Display（Cocoa point/y-up/左下原点 → 物理像素/y-down/左上原点）。
/// @param screen 目标屏幕。
/// @param primary 是否主屏。
/// @param primary_height 主屏 frame 高度（point，y 翻转基准）。
/// @return 折算后的显示器描述。
auto to_display(NSScreen *screen, bool primary, CGFloat primary_height) -> Display {
    Display d;
    NSDictionary *description = [screen deviceDescription];
    NSNumber *number = description != nil ? [description objectForKey:@"NSScreenNumber"] : nil;
    d.id = number != nil ? static_cast<int>([number unsignedIntValue]) : -1;
    NSString *name = [screen localizedName];
    if (name != nil) {
        const char *utf8 = [name UTF8String];
        if (utf8 != nullptr) {
            d.name = utf8;
        }
    }
    CGFloat scale = [screen backingScaleFactor];
    if (scale <= 0.0) {
        scale = 1.0;
    }
    // 折算闭包：Cocoa rect（y 向上）→ 物理像素 rect（y 向下）。
    const auto convert = [primary_height, scale](NSRect r) -> Rect {
        return Rect{.origin = Point{.x = static_cast<float>(r.origin.x * scale),
                                    .y = static_cast<float>((primary_height - r.origin.y - r.size.height) * scale)},
                    .size = Size{.width = static_cast<float>(r.size.width * scale),
                                 .height = static_cast<float>(r.size.height * scale)}};
    };
    d.bounds = convert(screen.frame);
    d.work_area = convert(screen.visibleFrame);
    d.scale_factor = static_cast<float>(scale);
    d.is_primary = primary;
    return d;
}

/// @brief 主屏高度（Cocoa point）；无屏幕时返回 0。
auto primary_height_points() -> CGFloat {
    NSArray<NSScreen *> *screens = [NSScreen screens];
    return (screens != nil && [screens count] > 0) ? [[screens objectAtIndex:0] frame].size.height
                                                   : static_cast<CGFloat>(0);
}

}  // namespace

auto list_displays() -> std::vector<Display> {
    std::vector<Display> out;
    @autoreleasepool {
        NSArray<NSScreen *> *screens = [NSScreen screens];
        const NSUInteger count = screens != nil ? [screens count] : 0;
        const CGFloat primary_height = primary_height_points();
        for (NSUInteger i = 0; i < count; ++i) {
            // `[NSScreen screens]` 的首项即主屏（AppKit 契约），与 `is_primary` 同源。
            out.push_back(to_display([screens objectAtIndex:i], i == 0, primary_height));
        }
    }
    if (out.empty()) {
        out.push_back(default_display());
    }
    return out;
}

auto primary_display() -> Display {
    @autoreleasepool {
        NSArray<NSScreen *> *screens = [NSScreen screens];
        if (screens != nil && [screens count] > 0) {
            return to_display([screens objectAtIndex:0], true, primary_height_points());
        }
    }
    return default_display();
}

auto move_window_to_display(Window &win, int display_id) -> void {
    auto *ns_window = static_cast<NSWindow *>(win.surface().native_handle());
    if (ns_window == nil) {
        return;  // 无头/未知后端：no-op
    }
    @autoreleasepool {
        NSScreen *target = nil;
        NSArray<NSScreen *> *screens = [NSScreen screens];
        for (NSUInteger i = 0; screens != nil && i < [screens count]; ++i) {
            NSScreen *screen = [screens objectAtIndex:i];
            NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
            // `NSScreenNumber` 是无符号 `CGDirectDisplayID`、`display_id` 有符号：按值比较而非
            // 折位比较，负 id 不再回绕成某个大键码而误命中。
            if ((number != nil) && std::cmp_equal([number unsignedIntValue], display_id)) {
                target = screen;
                break;
            }
        }
        if (target == nil) {
            target = (screens != nil && [screens count] > 0) ? [screens objectAtIndex:0] : nil;
        }
        if (target == nil) {
            return;
        }
        // 目标屏工作区（Cocoa point）居中；窗口尺寸原样保留（本函数只迁移、不改尺寸）。
        NSRect frame = [ns_window frame];
        const NSRect work = [target visibleFrame];
        frame.origin.x = work.origin.x + ((work.size.width - frame.size.width) * 0.5);
        frame.origin.y = work.origin.y + ((work.size.height - frame.size.height) * 0.5);
        [ns_window setFrameOrigin:frame.origin];
    }
}

auto display_containing(Point p) -> Display {
    for (const auto &d : list_displays()) {
        const Rect &b = d.bounds;
        if (p.x >= b.origin.x && p.x < b.right() && p.y >= b.origin.y && p.y < b.bottom()) {
            return d;
        }
    }
    return primary_display();
}

}  // namespace aurora::app

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
