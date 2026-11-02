#pragma once

#include <cstdint>

#include "aurora/environment/media_query.h"

namespace aurora {

/// @brief 主题风格轴：这套主题用哪套设计语言（与明暗轴 `ColorScheme` 正交）。
///
/// 明暗是「同一套设计语言的两种呈现」，风格是「换一套设计语言」，两轴各自独立取值：
/// 一枚 `ThemeStyle` 内含明暗两列标定值，与 `ColorRoles` 的「角色名一套、取值两列」同构。
/// 因此本枚举的取值**不含明暗位**——把暗色折成 `WindowsDark` 一类独立枚举值会让
/// `ColorScheme::Unspecified`（三级优先链的「本层不表态」态）无处安放。
///
/// 回落口径：`Material` 既是现行基线，也是**无证据环境的回落档**（无窗口后端、WASM、
/// 未映射进 `PlatformKind` 的目标且无桌面外壳证据）。本枚举不设 `Unknown`——风格轴必须
/// 有值才能出主题，「探测无证据」的表达方式是回落 `Material`，而不是把一枚不可用的取值
/// 塞进调用方的判空责任里。
enum class ThemeStyle : std::uint8_t {
    Material,  ///< Material 3 角色与字阶（现行基线，兼无证据环境的回落档）。
    Windows,  ///< Windows 11 / Fluent 视觉语言。
    MacOs,  ///< macOS / Aqua 视觉语言（拼写随 `PlatformKind::MacOs`）。
    Gnome,  ///< GNOME / Adwaita（libadwaita）视觉语言。
    Kde,  ///< KDE Plasma / Breeze 视觉语言。
};

/// @brief 桌面外壳（Linux/BSD 风格解算的输入维度；与 `PlatformKind` 的平台家族正交）。
///
/// `PlatformKind` 表 OS 家族，分不出同一台 Linux 机器跑的是 GNOME 还是 KDE，而这两者的
/// 观感应当不同——故风格探测需要第二枚证据。它**不进** `PlatformKind`（那是平台家族轴，
/// 扩值会牵动 `MediaQuery` 与既有断言）。
///
/// 证据口径（首个命中为准）：`XDG_CURRENT_DESKTOP`（冒号分隔）→ `DESKTOP_SESSION` →
/// 皆缺即 `Unknown`。未知外壳记 `Other`，与 `Unknown` 分列是为了让「环境说了但不认识」
/// 和「环境什么也没说」在诊断时可区分。
enum class DesktopEnvironment : std::uint8_t {
    Unknown,  ///< 无桌面环境证据（裸 X11 / 自建合成器 / 无会话变量）。
    Gnome,  ///< GNOME 及其它 GTK 外壳（Adwaita 视觉语言）。
    Kde,  ///< KDE Plasma（Breeze 视觉语言）。
    Other,  ///< 有证据但不认识的外壳。
};

namespace detail {

/// @brief 风格解算判据表（纯函数：证据进、风格出；不读环境、不碰缓存，故可逐格单测）。
///
/// 判据与理由：
/// - **无窗口后端 → `Material`**：无头构建（`SurfaceKind::Headless`）没有「这台机器的设计语言」
///   这件事可问，回落基线；渲染路径与 golden 基线因此保持确定性内建字体。
/// - **`Linux` 一行的 `Unknown` / `Other` 都取 `Gnome`**：Linux 上没有任何外壳证据时，本库既有的
///   GTK 配置启发式（`os_color_scheme()` 的 Linux 支读 `settings.ini`）已经默认这台机器在跑
///   GTK 系桌面，Adwaita 是与该启发式同源的基线，而不是另造一档「无主风格」。
/// - **`Unknown` 不等于「无风格可用」**：BSD / Android 等未映射进 `PlatformKind` 的目标仍可能带
///   XDG 会话证据，故有 KDE / GNOME 证据即取对应档，两者皆无才回落 `Material`。本提案不为
///   风格解算扩 `PlatformKind`（那是公共枚举扩值，会牵动 `MediaQuery` 与既有断言）。
/// - **`Web` 恒 `Material`**：浏览器没有桌面外壳概念；WASM 目标在 `platform()` 里其实落到
///   `Unknown`（`AURORA_PLATFORM_WASM` 与 `AURORA_PLATFORM_LINUX` 互斥），本分支是让「WASM →
///   Material」这条判据在读代码时当场可见，同时覆盖以 `MediaQuery` 为证据源的调用方。
/// @param kind 平台家族证据（须取 `platform().kind`，不是 `MediaQuery::platform`——后者表渲染后端）。
/// @param environment 桌面外壳证据（`probe_desktop_environment()` 的读数）。
/// @param has_window_backend 构建内是否存在真实窗口后端（`auto_detect_surface() != Headless`）。
/// @return 该证据组合的风格档；无证据时恒 `Material`，探测没有失败态。
[[nodiscard]] inline auto style_from(PlatformKind kind, DesktopEnvironment environment,
                                     bool has_window_backend) noexcept -> ThemeStyle {
    if (!has_window_backend) {
        return ThemeStyle::Material;
    }
    switch (kind) {
        case PlatformKind::Windows:
            return ThemeStyle::Windows;
        case PlatformKind::MacOs:
            return ThemeStyle::MacOs;
        case PlatformKind::Linux:
            return environment == DesktopEnvironment::Kde ? ThemeStyle::Kde : ThemeStyle::Gnome;
        case PlatformKind::Unknown:
            if (environment == DesktopEnvironment::Kde) {
                return ThemeStyle::Kde;
            }
            if (environment == DesktopEnvironment::Gnome) {
                return ThemeStyle::Gnome;
            }
            break;
        case PlatformKind::Web:
            break;
    }
    return ThemeStyle::Material;
}

/// @brief 读取桌面外壳证据（`DesktopEnvironment` 的唯一探测实现体，实现见探测 TU）。
///
/// 证据链（首个命中为准）：`XDG_CURRENT_DESKTOP`（冒号分隔，逐段匹配，取第一个认识的段）→
/// `DESKTOP_SESSION` → 两段都不认识记 `Other`（「环境说了但不认识」）→ 两段皆缺记 `Unknown`
/// （「环境什么也没说」）。认 KDE 的 `KDE` / `plasma`，认 GNOME 的 `GNOME` / `Unity` / `budgie` /
/// `cinnamon` / `pantheon` / `deepin`，比较一律忽略大小写。
///
/// 分支口径：只有 Linux / BSD 目标读进程环境——这两者的会话确实带上面两枚变量；其余目标
/// （Windows / macOS / WASM / Android / 无头）不读，直接 `Unknown`，它们的平台证据在
/// `PlatformKind` 那一轴，不该由环境变量表态。
/// @return 外壳证据；`default_style()` 调用它一次并进程内缓存。
/// @note Side-effects: reads the process environment（仅 Linux / BSD 分支）。
/// @note Thread: 无共享状态、无锁；静态局部量的一次性初始化由语言保证，读取路径无锁。
[[nodiscard]] auto probe_desktop_environment() -> DesktopEnvironment;

}  // namespace detail

}  // namespace aurora
