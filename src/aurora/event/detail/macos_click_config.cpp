// macos_click_config.cpp —— `os_double_click_ms()` 的 macOS 分支实现（ObjC++ TU）。
//
// 与 `win32_platform.cpp` / `macos_platform.cpp` 同定位：读取动作需要平台框架头，而消费方
// （`src/aurora/event/click_config.cpp`）是纯 C++，故声明留在内部头、AppKit 调用落在本 TU，
// 语言由 `cmake/AuroraBackends.cmake` 的 macOS ObjC++ 清单显式指定。
//
// 读取口径：`+[NSEvent doubleClickInterval]` 是 AppKit 对外发布的「双击速度」判定入口，返回**秒**
// （System Settings → Mouse 的 Double-click speed 滑杆折算值，本机实测默认 0.5s）。它是类方法、
// 不要求 `NSApplication` 已初始化，故无头/CLI 进程同样读得到（本机以裸 Swift 可执行进程实测）。
// 每次调用现读、不做进程级缓存——与 Windows `GetDoubleClickTime()` 支同语义：运行期改系统设置后
// 重建派发器即反映新值。折算规则（非正 / 非有限 → 库级兜底，亚毫秒 → 夹 1）不在本文件，
// 见 `macos_click_config.h` 的纯函数，理由是该折算要在非 Apple runner 上表驱动单测。

// 平台宏须先于下方守卫可见：本 TU 的其余 include 全在守卫内，否则守卫恒假、整个 TU 编译为空。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)  // NOLINT

#import <AppKit/AppKit.h>

#include "aurora/event/detail/macos_click_config.h"
#include "aurora/event/event.h"

namespace aurora::detail {

auto macos_double_click_ms() -> std::uint32_t {
    const NSTimeInterval seconds = [NSEvent doubleClickInterval];
    return click_window_ms_from_seconds(static_cast<double>(seconds), AURORA_DEFAULT_CLICK_WINDOW_MS);
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
