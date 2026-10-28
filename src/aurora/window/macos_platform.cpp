// macos_platform.cpp — `os_color_scheme()` 的 macOS 分支实现。
//
// 为何独立 TU：`os_color_scheme()` 是公共头（`window/platform.h`）里的 inline 函数，其
// macOS 分支要问 AppKit 的 `NSAppearance`，公共头不能引入 `<AppKit/AppKit.h>`（会污染
// 所有消费者的编译面）。故声明留在公共头、实现落在本 TU——与 `win32_platform.cpp`
// 的注册表读取同一定位。
//
// 读取口径：`NSApp.effectiveAppearance` 的 bestMatch 是 AppKit 对外发布的明暗判定入口；
// `NSAppearanceNameDarkAqua` 命中即 Dark，Aqua 命中即 Light。应用尚未初始化 AppKit
// （`NSApp == nil`，如 CLI/无头场景）时返回 `Unspecified`——无证据不猜，交由调用方回落。
//
// 门控：macOS 平台且 macOS 后端（公共头只在同一组合下调用本实现；后端关闭时该分支
// 编译为 `Unspecified`，不会产生未定义符号）。

#include "aurora/core/platform.h"  // NOLINT
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

#import <AppKit/AppKit.h>

#include "aurora/window/platform.h"

namespace aurora::detail {

auto macos_os_color_scheme() -> ColorScheme {
    @autoreleasepool {
        NSAppearance *appearance = [NSApp effectiveAppearance];
        if (appearance == nil) {
            return ColorScheme::Unspecified;  // AppKit 未初始化：不是「明色」，是「不知道」
        }
        NSAppearanceName best = [appearance bestMatchFromAppearancesWithNames:@[
            NSAppearanceNameAqua, NSAppearanceNameDarkAqua
        ]];
        if (best == nil) {
            return ColorScheme::Unspecified;
        }
        return [best isEqualToString:NSAppearanceNameDarkAqua] ? ColorScheme::Dark : ColorScheme::Light;
    }
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
