#pragma once
#include "aurora/core/platform.h"
#include "aurora/environment/media_query.h"
#include "aurora/window/window.h"

namespace aurora {

/// @brief 平台能力标志（显式查询，跳过 Surface 探测；specification/06-app-platform.md §5）。
struct PlatformCapabilities {
    bool multitouch = false;  ///< 支持多点触控（真实显示 Surface Win32/Glfw 为 true，Headless 为 false）。
    bool high_frequency_pointer = false;  ///< 高频率指针采样（Win32/Glfw 为 true）。
    bool desktop = false;  ///< 桌面形态。
    bool mobile = false;  ///< 移动形态（Mobile/Tablet）。
};

/// @brief 平台与运行环境快照（显式查询，跳过 Surface 探测，specification/06-app-platform.md §5）。
/// 平台家族 `PlatformKind` 与设备形态 `DeviceKind` 复用 `MediaQuery` 既有定义，避免重复。
struct Platform {
    /// @brief 平台家族（Windows/MacOs/Linux/Unknown）；由 `platform()` 按编译期 OS 宏填充。
    PlatformKind kind = PlatformKind::Unknown;
    /// @brief 设备形态；当前各已知桌面 OS 均推断为 Desktop，未知平台保持 Unknown。
    DeviceKind device = DeviceKind::Unknown;
    /// @brief 实际可用的 Surface 后端类别；由 `platform()` 经 `auto_detect_surface()` 探测填充。
    SurfaceKind surface = SurfaceKind::Headless;

    /// @brief 是否为移动形态设备。
    /// @return device 为 Mobile 或 Tablet 时 true，否则 false。
    [[nodiscard]] auto is_mobile() const -> bool {
        return device == DeviceKind::Mobile || device == DeviceKind::Tablet;
    }
    /// @brief 是否为桌面形态设备。
    /// @return device 为 Desktop 时 true，否则 false。
    [[nodiscard]] auto is_desktop() const -> bool { return device == DeviceKind::Desktop; }

    /// @brief 由设备形态与 Surface 后端类别推导平台能力标志。
    /// @return 聚合结果：desktop/mobile 按 DeviceKind 判定；multitouch 与 high_frequency_pointer
    /// 在 Win32/Glfw/X11/Wayland/MacOS 五类真实显示 Surface 下为 true。
    [[nodiscard]] auto capabilities() const -> PlatformCapabilities {
        // 待填充的能力标志聚合：默认值起步，先按设备形态、再按 SurfaceKind 逐项置位。
        PlatformCapabilities c;
        c.desktop = is_desktop();
        c.mobile = is_mobile();
        // `SurfaceKind` 所有枚举器无条件存在（见 `window.h` 稳定性契约），此处比较不再受
        // `AURORA_BACKEND_*` 宏影响；未编译的后端其标签只是运行期不会被产出，比较恒为 false。
        c.multitouch = (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw || surface == SurfaceKind::X11 ||
                        surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        c.high_frequency_pointer =
            (surface == SurfaceKind::Win32 || surface == SurfaceKind::Glfw || surface == SurfaceKind::X11 ||
             surface == SurfaceKind::Wayland || surface == SurfaceKind::MacOS);
        return c;
    }
};

/// @brief 显式查询当前平台与运行环境（编译期 OS + 自动探测 Surface，specification/06-app-platform.md §5）。
/// 不构造任何 `Window` / `Surface`，保持 widget 不反向依赖 Surface 分层。
/// @return Platform 快照：surface 为自动探测结果，kind/device 按编译期 OS 宏推断
/// （Windows/MacOs/Linux→Desktop，其余→Unknown）。
[[nodiscard]] inline auto platform() -> Platform {
    Platform p;
    p.surface = auto_detect_surface();
#ifdef AURORA_PLATFORM_WINDOWS
    p.kind = PlatformKind::Windows;
    p.device = DeviceKind::Desktop;
#elif defined(AURORA_PLATFORM_MACOS)
    p.kind = PlatformKind::MacOs;
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

}  // namespace aurora
