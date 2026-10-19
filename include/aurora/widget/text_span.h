#pragma once

#include <optional>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/theming/theme_scope.h"  // inherit_theme：未显式设色时取主题角色

namespace aurora {

/// @brief 富文本片段（内联样式单元）。
///
/// 一段拥有**统一字体/颜色**的文本，多个 `TextSpan` 顺序拼接即构成富文本。
/// `text` 为 `LocalizedString`，渲染时经 `default_string_table()` + 当前 `Locale` 解析，支持 i18n。
struct TextSpan {
    LocalizedString text;  ///< 片段文本（支持 i18n 查表）
    Font font = Font{};  ///< 字体（字号 / 字重 / 字族）
    /// @brief 文本颜色；空 = 跟随主题 `on_surface`（ThemeScope 换肤即生效，与 Text 同口径）。
    ///        显式赋色优先，跨入/换肤时未显式设色的片段自动跟随当前主题角色。
    std::optional<Color> color;

    /// @brief 解析生效颜色：未显式设色时经 ctx 取主题 `on_surface`（ThemeScope 换肤即生效），
    ///        显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light（与 OS 未表态同口径）。
    /// @return 生效颜色（已解析主题角色或显式色）。
    [[nodiscard]] auto resolve_color(const BuildContext &ctx) const -> Color {
        return color.value_or(inherit_theme(ctx).roles.on_surface);
    }
};

}  // namespace aurora
