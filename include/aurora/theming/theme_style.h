#pragma once

#include <cstdint>

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

}  // namespace aurora
