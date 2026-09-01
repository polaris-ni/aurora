#pragma once
#include "aurora/core/platform.h"
#include "aurora/environment/media_query.h"
#include "aurora/window/window.h"

namespace aurora {

/// @brief 平台能力标志（显式查询，跳过 Surface 探测；specification/06-app-platform.md §5）。
struct PlatformCapabilities {
    bool multitouch = false;             ///< 支持多点触控（真实显示 Surface Win32/Glfw 为 true，Headless 为 false）。
    bool high_frequency_pointer = false; ///< 高频率指针采样（Win32/Glfw 为 true）。
    bool desktop = false;                ///< 桌面形态。
    bool mobile = false;                 ///< 移动形态（Mobile/Tablet）。
};

/// @brief 平台与运行环境快照（显式查询，跳过 Surface 探测，specification/06-app-platform.md §5）。
/// 平台家族 `PlatformKind` 与设备形态 `DeviceKind` 复用 `MediaQuery` 既有定义，避免重复。
struct Platform {
    PlatformKind kind = PlatformKind::Unknown;
    DeviceKind device = DeviceKind::Unknown;
    SurfaceKind surface = SurfaceKind::Headless;

    [[nodiscard]] auto is_mobile() const -> bool {
        return device == DeviceKind::Mobile || device == DeviceKind::Tablet;
    }
    [[nodiscard]] auto is_desktop() const -> bool { return device == DeviceKind::Desktop; }

    [[nodiscard]] auto capabilities() const -> PlatformCapabilities {
        PlatformCapabilities c;
        c.desktop = is_desktop();
        c.mobile = is_mobile();
        c.multitouch = (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw
#ifdef AURORA_BACKEND_X11
                        || surface == SurfaceKind::X11
#endif
#ifdef AURORA_BACKEND_WAYLAND
                        || surface == SurfaceKind::Wayland
#endif
#ifdef AURORA_BACKEND_MACOS
                        || surface == SurfaceKind::MacOS
#endif
        );
        c.high_frequency_pointer = (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw
#ifdef AURORA_BACKEND_X11
                                    || surface == SurfaceKind::X11
#endif
#ifdef AURORA_BACKEND_WAYLAND
                                    || surface == SurfaceKind::Wayland
#endif
#ifdef AURORA_BACKEND_MACOS
                                    || surface == SurfaceKind::MacOS
#endif
        );
        return c;
    }
};

/// @brief 显式查询当前平台与运行环境（编译期 OS + 自动探测 Surface，specification/06-app-platform.md §5）。
/// 不构造任何 `Window` / `Surface`，保持 widget 不反向依赖 Surface 分层。
[[nodiscard]] inline auto platform() -> Platform {
    Platform p;
    p.surface = auto_detect_surface();
#ifdef AURORA_PLATFORM_WINDOWS
    p.kind = PlatformKind::Windows;
    p.device = DeviceKind::Desktop;
#elif defined(AURORA_PLATFORM_MACOS)
    p.kind = PlatformKind::macOS;
    p.device = DeviceKind::Desktop;
#elif defined(AURORA_PLATFORM_LINUX)
    p.kind = PlatformKind::Linux;
    p.device = DeviceKind::Desktop;
#else
    p.kind = PlatformKind::Unknown;
    p.device = DeviceKind::Unknown;
#endif
    return p;
}

} // namespace aurora
