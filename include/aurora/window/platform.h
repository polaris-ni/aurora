#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

#include "aurora/core/platform.h"
#include "aurora/environment/media_query.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/window/window.h"

namespace aurora {

/// @brief 平台能力标志（显式查询，跳过 Surface 探测；specification/06-app-platform.md §5）。
struct PlatformCapabilities {
    bool multitouch = false;  ///< 支持多点触控（真实显示 Surface Win32/Glfw 为 true，Headless 为 false）。
    bool high_frequency_pointer = false;  ///< 高频率指针采样（Win32/Glfw 为 true）。
    bool desktop = false;  ///< 桌面形态。
    bool mobile = false;  ///< 移动形态（Mobile/Tablet）。
    /// @brief 支持接收系统拖入（跨进程拖放进应用）。
    ///
    /// 能力位只表「机制是否存在」：运行期不可用（会话建立失败、权限不足等）仍走 `Result` 错误，
    /// 二者不互相替代。**当前恒为 false**——系统拖放后端尚未接线，此处不预先声称支持；接线后按各
    /// 后端实测翻位。
    bool system_drag_in = false;
    /// @brief 支持发起系统拖出（从应用拖到其它进程）。语义同上，**当前恒为 false**。
    bool system_drag_out = false;
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

    /// @brief OS 明暗偏好快照；默认 `Unspecified`，须由调用方经 `os_color_scheme()` 显式填充。
    ///
    /// 与本结构其余探测字段不同：读 OS 偏好有文件/注册表访问副作用，`platform()` 不代做，
    /// 保持「显式查询，跳过探测」的口径（见 `os_color_scheme()`）。
    ColorScheme color_scheme = ColorScheme::Unspecified;

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
    /// 在 Win32/Glfw/X11/Wayland/MacOS 五类真实显示 Surface 下为 true；system_drag_in /
    /// system_drag_out 恒为 false（系统拖放后端尚未接线，保持默认值即「不声称支持」）。
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

#ifdef AURORA_PLATFORM_WINDOWS
namespace detail {
/// @brief Win32 注册表读取 OS 明暗偏好（`os_color_scheme` 的 Windows 分支实现）。
/// 定义见 `src/aurora/window/win32_platform.cpp`——公共头不引入 `<windows.h>`。
/// @return Dark/Light；打不开注册表键时 `Unspecified`。
[[nodiscard]] auto win32_os_color_scheme() -> ColorScheme;
}  // namespace detail
#endif

/// @brief 显式查询 OS 明暗偏好（明暗三级优先链的最低层）。
///
/// 各平台实现与局限：
/// - **Linux**：GTK 配置启发式——读 `$XDG_CONFIG_HOME/gtk-4.0/settings.ini`（缺省
///   `~/.config/...`），`gtk-application-prefer-dark-theme=true` 或 `gtk-theme` 含
///   `-dark` 后缀判 Dark，其余判 Light。**局限**：不查询 xdg-desktop-portal 的
///   `org.freedesktop.appearance`（异步 DBus，库侧不引入）；非 GTK 桌面可能误报。
/// - **Windows**：读注册表 `HKCU\...\Themes\Personalize\AppsUseLightTheme`（REG_DWORD，
///   0=Dark、1=Light）。键不存在（Win10 1803 之前）按 Light；打不开键返回 `Unspecified`。
///   无缓存轮询（每次直读，OS 偏好可能被用户随时更改）。
/// - **macOS**：契约 = `NSAppearance` bestMatch（Dark→Dark）。
///   **未在本仓库无头构建内编译验证。**
/// - 其余平台：`Unspecified`。
/// @return OS 档位；无证据时不猜，返回 `Unspecified` 交由调用方回落。
[[nodiscard]] inline auto os_color_scheme() -> ColorScheme {
#ifdef AURORA_PLATFORM_WINDOWS
    // 实现见 src/aurora/window/win32_platform.cpp：注册表查询留在此处会把 <windows.h>
    // 拖进公共头（min/max 宏污染跨平台消费者），故声明在下、实现在独立 TU。
    return detail::win32_os_color_scheme();
#elif defined(AURORA_PLATFORM_LINUX)
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string base =
        (xdg != nullptr && *xdg != '\0') ? std::string(xdg) : std::string(home == nullptr ? "" : home) + "/.config";
    for (const char *ver : {"gtk-4.0", "gtk-3.0"}) {
        std::ifstream in(base + "/" + ver + "/settings.ini");
        if (!in.is_open()) {
            continue;
        }
        std::string line;
        bool prefer_dark = false;
        bool dark_theme = false;
        bool any_line = false;
        while (std::getline(in, line)) {
            any_line = true;
            if (line.find("gtk-application-prefer-dark-theme") != std::string::npos &&
                line.find("true") != std::string::npos) {
                prefer_dark = true;
            }
            if (line.find("gtk-theme") != std::string::npos && line.find("-dark") != std::string::npos) {
                dark_theme = true;
            }
        }
        if (any_line) {
            return (prefer_dark || dark_theme) ? ColorScheme::Dark : ColorScheme::Light;
        }
    }
    return ColorScheme::Unspecified;
#elif defined(AURORA_PLATFORM_MACOS)
    // 见函数头契约注记：NSAppearance 查询未在本仓库无头构建内编译验证。
    return ColorScheme::Unspecified;
#else
    return ColorScheme::Unspecified;
#endif
}

}  // namespace aurora
