#pragma once

#include "aurora/theming/color_scheme.h"
#include "aurora/theming/theme.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window_frame_style.h"

namespace aurora {

/// @brief 明暗三级优先解析（纯函数）：显式配置 > `Theme` 令牌（`Theme::scheme`）> OS 偏好。
///
/// 每层以 `ColorScheme::Unspecified` 表示「该层没有意见」，逐层下探；全链未指定时返回
/// `Unspecified`，由调用方回落默认档（本库统一口径：`Unspecified` ≡ 明色列，见下与
/// specification/07-environment-modifier.md §5）。
/// @param explicit_scheme 应用显式指定的档位（最高优先）。
/// @param theme_scheme 生效 `Theme::scheme`（中间层）。
/// @param os_scheme OS 偏好（`os_color_scheme()`，最低优先）。
/// @return 解析结果；三层都未表态时为 `Unspecified`。
[[nodiscard]] inline auto resolve_color_scheme(ColorScheme explicit_scheme, ColorScheme theme_scheme,
                                               ColorScheme os_scheme) -> ColorScheme {
    if (explicit_scheme != ColorScheme::Unspecified) {
        return explicit_scheme;
    }
    if (theme_scheme != ColorScheme::Unspecified) {
        return theme_scheme;
    }
    return os_scheme;
}

/// @brief 从生效主题派生 CSD 标题栏样式（`Theme::csd` → `TitleBarStyle` 的 typed 派生通道）。
///
/// 两段式取值：
/// 1. **预设基座**：按 `theme.scheme` 取 `CsdPreset::AdwaitaLight` / `AdwaitaDark`——`Unspecified`
///    按明色（与「`Unspecified` ≡ 明色列」全系统单一口径一致）；保证未逐字段定制时，派生结果与直接
///    取预设逐值相同；
/// 2. **配色字段**：从 `theme.csd` 角色组直接取（`CsdRoles::light()` / `dark()` 的取值 ≡ 对应
///    Adwaita 预设，故工厂主题派生即等于预设；应用改 `theme.csd` 某字段即逐字段定制）。
///
/// 栏高 / 布局语言 / 显隐 / 度量等几何与行为字段不参与派生（属定制面，维持 CHG-004 口径）。
/// @param t 生效主题（`scheme` 为最终档、`csd` 为对应列或由应用逐字段覆写）。
/// @return 派生样式。
[[nodiscard]] inline auto title_bar_style_from_theme(const Theme &t) -> TitleBarStyle {
    const CsdPreset base = (t.scheme == ColorScheme::Dark) ? CsdPreset::AdwaitaDark : CsdPreset::AdwaitaLight;
    TitleBarStyle s = TitleBarStyle::from_preset(base);
    s.bg_active = t.csd.title_bar_bg_active;
    s.bg_inactive = t.csd.title_bar_bg_inactive;
    s.fg_active = t.csd.title_bar_fg_active;
    s.fg_inactive = t.csd.title_bar_fg_inactive;
    s.hover_tint = t.csd.title_bar_hover_tint;
    s.pressed_tint = t.csd.title_bar_pressed_tint;
    s.close_hover = t.csd.title_bar_close_hover;
    return s;
}

/// @brief 从生效主题派生窗框样式（仅投影配色；厚度/模糊的几何默认值不在角色面内）。
/// @param t 生效主题。
/// @param base 基座（缺省 = `WindowFrameStyle{}`，即既有常量基线）；仅投影基色取
///     `theme.csd.shadow_color`，厚度与模糊半径保持基座值——改厚度会牵动缩放热区与 buffer 几何，
///     属行为定制而非配色，须走显式 `WindowFrameStyle`。
/// @return 派生样式。
[[nodiscard]] inline auto window_frame_style_from_theme(const Theme &t, const WindowFrameStyle &base = {})
    -> WindowFrameStyle {
    WindowFrameStyle f = base;
    f.shadow_color = t.csd.shadow_color;
    return f;
}

}  // namespace aurora
