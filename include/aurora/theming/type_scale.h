#pragma once

#include "aurora/core/font.h"

namespace aurora {

/// @brief 排版角色集：Material 3 文字阶（type scale）15 档，每档一个 `Font`。
///
/// 取代旧 `Theme::font` 单一正文字段。字阶不分明暗档（排印与配色解耦），故 `Theme` 只持一列
/// `TypeScale`。成员默认值即 M3 标准字阶；旧 `Theme::font` 的语义由 `body_medium`（14pt Regular，
/// 恰与默认构造的 `Font{}` 逐字段相同）承接——读 `theme.font` 的图表族机械迁移至
/// `theme.type_scale.body_medium`，观感逐位不变。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct TypeScale {
    Font display_large{Font{.size_pt = 57.0F, .weight = 400}};  ///< Display Large。
    Font display_medium{Font{.size_pt = 45.0F, .weight = 400}};  ///< Display Medium。
    Font display_small{Font{.size_pt = 36.0F, .weight = 400}};  ///< Display Small。
    Font headline_large{Font{.size_pt = 32.0F, .weight = 400}};  ///< Headline Large。
    Font headline_medium{Font{.size_pt = 28.0F, .weight = 400}};  ///< Headline Medium。
    Font headline_small{Font{.size_pt = 24.0F, .weight = 400}};  ///< Headline Small。
    Font title_large{Font{.size_pt = 22.0F, .weight = 400}};  ///< Title Large。
    Font title_medium{Font{.size_pt = 16.0F, .weight = 500}};  ///< Title Medium。
    Font title_small{Font{.size_pt = 14.0F, .weight = 500}};  ///< Title Small。
    Font body_large{Font{.size_pt = 16.0F, .weight = 400}};  ///< Body Large。
    Font body_medium{Font{.size_pt = 14.0F, .weight = 400}};  ///< Body Medium（旧正文默认字体；图表族迁移至此）。
    Font body_small{Font{.size_pt = 12.0F, .weight = 400}};  ///< Body Small。
    Font label_large{Font{.size_pt = 14.0F, .weight = 500}};  ///< Label Large。
    Font label_medium{Font{.size_pt = 12.0F, .weight = 500}};  ///< Label Medium。
    Font label_small{Font{.size_pt = 11.0F, .weight = 500}};  ///< Label Small。
};

}  // namespace aurora
