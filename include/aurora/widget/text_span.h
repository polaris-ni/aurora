#pragma once

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/i18n/localized_string.h"

namespace aurora {

/**
 * @brief 富文本片段（内联样式单元）。
 *
 * 一段拥有**统一字体/颜色**的文本，多个 `TextSpan` 顺序拼接即构成富文本。
 * `text` 为 `LocalizedString`，渲染时经 `defaultStringTable` + 当前 `Locale` 解析，支持 i18n。
 */
struct TextSpan {
    LocalizedString text;         ///< 片段文本（支持 i18n 查表）
    Font font = Font{};           ///< 字体（字号 / 字重 / 字族）
    Color color = Color::black(); ///< 文本颜色
};

} // namespace aurora
