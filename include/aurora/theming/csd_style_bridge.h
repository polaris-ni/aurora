#pragma once

#include <optional>

#include "aurora/theming/color_scheme.h"
#include "aurora/theming/theme.h"
#include "aurora/window/title_bar_style.h"
#include "aurora/window/window_frame_style.h"

namespace aurora {

/// @brief 明暗三级优先解析（纯函数）：显式配置 > `Theme` 令牌 > OS 偏好。
///
/// 每层以 `ColorScheme::Unspecified` 表示「该层不表态」，逐层下探；全链未指定时
/// 返回 `Unspecified`，由调用方回落预设默认档（`CsdPreset::AdwaitaDark`）。
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

/// @brief 从生效主题派生 CSD 标题栏样式（`Theme` → `TitleBarStyle` 的令牌派生通道）。
///
/// 三段式取值：
/// 1. **预设基座**：按 `theme.scheme` 取 `CsdPreset::AdwaitaLight` / `AdwaitaDark`
///    （`Unspecified` 按暗色——与 `TitleBarStyle{}` 默认观感一致）；保证未覆写任何
///    令牌时，派生结果与直接取预设逐值相同；
/// 2. **组件覆写位**：`csd.title_bar.*` 令牌存在则逐项覆盖（想改哪处写哪处）；
/// 3. 其余字段（栏高 / 布局语言 / 显隐开关 / 度量覆写）不参与派生，保持预设值——
///    几何与行为属定制面，不属配色体系。
///
/// 与显式样式的关系（优先链第一层）：调用方先 `resolve_color_scheme` 定档、后构
/// `Theme`，或对派生结果再做字段级改写——本函数只承载「令牌 → 样式」这一段。
/// @param t 生效主题（须已按三级优先链解析出档位，即 `t.scheme` 为最终档）。
/// @return 派生样式。
[[nodiscard]] inline auto title_bar_style_from_theme(const Theme &t) -> TitleBarStyle {
    const CsdPreset base = (t.scheme == ColorScheme::Light) ? CsdPreset::AdwaitaLight : CsdPreset::AdwaitaDark;
    TitleBarStyle s = TitleBarStyle::from_preset(base);
    if (auto v = t.token_for("csd.title_bar.bg.active")) {
        if (auto c = v->as<Color>()) {
            s.bg_active = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.bg.inactive")) {
        if (auto c = v->as<Color>()) {
            s.bg_inactive = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.fg.active")) {
        if (auto c = v->as<Color>()) {
            s.fg_active = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.fg.inactive")) {
        if (auto c = v->as<Color>()) {
            s.fg_inactive = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.hover")) {
        if (auto c = v->as<Color>()) {
            s.hover_tint = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.pressed")) {
        if (auto c = v->as<Color>()) {
            s.pressed_tint = *c;
        }
    }
    if (auto v = t.token_for("csd.title_bar.close.hover")) {
        if (auto c = v->as<Color>()) {
            s.close_hover = *c;
        }
    }
    return s;
}

/// @brief 从生效主题派生窗框样式（投影配色；厚度/模糊的几何默认值不在令牌面内）。
/// @param t 生效主题。
/// @param base 基座（缺省 = `WindowFrameStyle{}`，即既有常量基线）；仅投影基色可被
///     `csd.shadow.color` 令牌覆写，厚度与模糊半径保持基座值——改厚度会牵动缩放
///     热区与 buffer 几何，属行为定制而非配色，须走显式 `WindowFrameStyle`。
/// @return 派生样式。
[[nodiscard]] inline auto window_frame_style_from_theme(const Theme &t, const WindowFrameStyle &base = {}) -> WindowFrameStyle {
    WindowFrameStyle f = base;
    if (auto v = t.token_for("csd.shadow.color")) {
        if (auto c = v->as<Color>()) {
            f.shadow_color = *c;
        }
    }
    return f;
}

}  // namespace aurora
