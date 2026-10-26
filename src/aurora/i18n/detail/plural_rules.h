#pragma once

/// @internal plural_rules.gen.cpp 的私有接口：CLDR 规则判定表（导出件）只经由这里暴露给库内。
///
/// 数据来源与再生成命令见 `src/aurora/i18n/plural_rules.gen.cpp` 头注释与
/// `THIRD_PARTY_LICENSES.md` §10；本头不属于公共 API，禁止被 include/ 下的公共头引用。

#include <cstdint>
#include <string_view>

#include "aurora/i18n/plural.h"

namespace aurora::detail {

/// @brief 语言键 → 规则行下标（二分查找）。
///
/// 键为全小写语言码（CLDR 48 数据集均为纯语言码，无 region 变体键）；未收录返回 -1。
/// @param key 语言键（如 "en"）。
/// @return 规则行下标；未收录 -1。
[[nodiscard]] auto plural_rules_language_index(std::string_view key) -> int;

/// @brief 取某语言行收录的类别掩码（bit0=Zero … bit4=Many；Other 恒隐含）。
/// @param index 规则行下标（-1 得 0）。
/// @param ordinal true 取序数表，false 取基数表。
/// @return 类别位掩码。
[[nodiscard]] auto plural_rules_category_mask(int index, bool ordinal) -> std::uint32_t;

/// @brief 按 CLDR 基数规则判定类别（首条命中即返回，全不命中回 Other）。
/// @param index 规则行下标（-1 直接 Other）。
/// @param ops 复数操作数。
/// @return CLDR 基数类别。
[[nodiscard]] auto plural_rules_eval_cardinal(int index, const PluralOperands &ops) -> PluralCategory;

/// @brief 按 CLDR 序数规则判定类别（首条命中即返回，全不命中回 Other）。
/// @param index 规则行下标（-1 直接 Other）。
/// @param ops 复数操作数。
/// @return CLDR 序数类别。
[[nodiscard]] auto plural_rules_eval_ordinal(int index, const PluralOperands &ops) -> PluralCategory;

}  // namespace aurora::detail
