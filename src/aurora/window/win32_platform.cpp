#include "aurora/window/platform.h"

// win32_platform.cpp — `platform.h` 内联函数的 Windows 侧实现载体。
//
// 为何独立 TU：`os_color_scheme()` 是公共头里的 inline 函数，其 Win32 分支要读注册表
// （advapi32）。若把 <windows.h> 直接塞进 platform.h，所有跨平台消费者都会被拖进
// windows.h 的宏污染面（NOMINMAX 未定义时 min/max 宏会击穿用户代码）。故声明留在
// 公共头、实现落在本 TU——与 Win32Host 的 pimpl 隔离同一纪律。
#ifdef AURORA_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// clang-format off
#include <windows.h>
// clang-format on

namespace aurora::detail {

auto win32_os_color_scheme() -> ColorScheme {
    // 契约（platform.h 头注）：HKCU\...\Themes\Personalize\AppsUseLightTheme，REG_DWORD，
    // 0 = Dark、1 = Light。Win10 1803 之前该键不存在——按 Light 处理（明暗主题是后来的
    // 概念，老系统即等效 Light）。打不开键（策略封锁等）不猜，返回 Unspecified 交调用方
    // 按其三级优先链回落。无缓存轮询：每次调用直读，与「OS 偏好可能被用户随时更改」
    // 的语义一致（读取成本一次注册表查询，可忽略）。
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return ColorScheme::Unspecified;
    }
    DWORD value = 1;  // 缺省按 Light（键在值缺 = 未表达偏好）
    DWORD size = sizeof(value);
    DWORD type = 0;
    // Win32 注册表 API 以 BYTE* 承载 DWORD 缓冲：协议约定的宽度重解释，非别名违规。
    const LSTATUS status =
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                         &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) {
        return ColorScheme::Light;
    }
    return value == 0 ? ColorScheme::Dark : ColorScheme::Light;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS
