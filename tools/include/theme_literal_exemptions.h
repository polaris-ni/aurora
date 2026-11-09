// ============================================================================
// theme_literal_exemptions.h - single source for allowed numeric color literals
// ----------------------------------------------------------------------------
// 验收判据「主题债务清零」要求控件层与 CSD 预设表里的 `Color{...}` / `Color(...)` 数字构造式
// 归零；仍允许存在的每一处必须在这里逐条登记**文件 + 精确出现次数 + 理由**。本表同时是：
//   1. `aurora_api.json` 顶层 `theme_tokens.exemptions.files` 的发射源（gen_api 编译期读本表，
//      故 stdout 与写文件两条调用路径发射内容一致）；
//   2. `tools/check/check_no_theme_color_literals.py` 的判据表（脚本按同样三列解析本文件）。
// 新增豁免 = 同时改本表与理由；计数漂一处即红，不允许「按文件放行」。
// 理由串必须是 ASCII（字面量语言纪律），解释「为什么它不是待清的主题债务」。
// ============================================================================
#pragma once

#include <array>
#include <string_view>

namespace aurora::tools {

/// @brief 一条颜色字面量豁免：仓库相对路径 + 允许出现次数 + 理由。
struct ThemeColorLiteralExemption {
    std::string_view path;  ///< 仓库相对路径（与门禁扫描口径一致）。
    int count;  ///< 该文件允许的 `Color{...}` / `Color(...)` 数字构造式出现次数。
    std::string_view reason;  ///< 为什么这些色值不属主题债务（ASCII 英文）。
};

/// @brief 控件层与 CSD 预设表的颜色字面量豁免表（唯一真源）。
inline constexpr std::array<ThemeColorLiteralExemption, 4> AURORA_THEME_COLOR_LITERAL_EXEMPTIONS{{
    {.path = "include/aurora/widget/chart_common.h",
     .count = 16,
     .reason = "chart series palettes: built-in fallback and ChartPalette default share eight data-encoding "
               "colours, not theme roles; the two tables must stay equal so an unseeded theme renders identically"},
    {.path = "include/aurora/widget/pickers.h",
     .count = 17,
     .reason = "ColorPicker swatch data: the default sixteen-swatch palette and one descriptor example string "
               "are the picker payload, not component chrome"},
    {.path = "include/aurora/widget/recipes.h",
     .count = 3,
     .reason = "composition recipes build Modifier backgrounds before any BuildContext exists, so the colour is "
               "fixed at composition time and cannot read theme roles"},
    {.path = "include/aurora/window/title_bar_style.h",
     .count = 40,
     .reason = "CsdPreset tables: the per-style title-bar look-alike values are the source CsdRoles is derived "
               "from, not widget chrome that should read roles"},
}};

}  // namespace aurora::tools
