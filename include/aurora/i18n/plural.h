#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "aurora/i18n/locale.h"

namespace aurora {

/// @brief CLDR 复数操作数（TR35 定义的判定输入，从计数值的十进制表示推导）。
///
/// - `n`：绝对值（浮点视图）
/// - `i`：整数部分
/// - `v`：可见小数位数（含尾随零，如 "1.20" → 2）
/// - `w`：去尾随零的小数位数（"1.20" → 1）
/// - `f`/`t`：小数位数字组成的整数（含/不含尾随零，"1.20" → 20/2）
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct PluralOperands {
    double n = 0.0;  ///< 绝对值
    long long i = 0;  ///< 整数部分
    int v = 0;  ///< 可见小数位数（含尾随零）
    int w = 0;  ///< 去尾随零的小数位数
    long long f = 0;  ///< 小数位数字组成的整数（含尾随零）
    long long t = 0;  ///< 小数位数字组成的整数（去尾随零）
};

/// @brief CLDR 复数类别（cardinal，基数）。
///
/// 六类与 CLDR 定义一致：每个语言至少含 `Other`，其余类别仅在该语言确有不同措辞时才出现。
/// 数值到类别的映射由 `plural_category` / `plural_category_ordinal` 经 CLDR 导出判定表完成
/// （`src/aurora/i18n/plural_rules.gen.cpp`，CLDR 48 全语言，见 THIRD_PARTY_LICENSES.md §10）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
/// @note 豁免 performance-enum-size：公共 API 枚举，作 plural_category 的返回值，仅作 switch/临时量，
/// 不落在结构体或容器字段里；底层类型是公共 API 形态的一部分，本库按语义选型而非体积取向，改窄仅省 3 字节。
/// NOLINTNEXTLINE(performance-enum-size)
enum class PluralCategory {
    Zero,  ///< CLDR zero：如阿拉伯语 0
    One,  ///< CLDR one：如英语 1、法语 0/1、俄语 1/21/101（非 11）
    Two,  ///< CLDR two：如阿拉伯语 2（双数）
    Few,  ///< CLDR few：如俄语 2-4（非 12-14）、阿拉伯语 3-10
    Many,  ///< CLDR many：如俄语 0/5-20、阿拉伯语 11-99
    Other,  ///< CLDR other：兜底类别，所有语言必备
};

/// @brief 由整数构造复数操作数（无小数部分，v/w/f/t 全零）。
/// @param value 整数值（符号不参与分类，内部取绝对值）。
/// @return 对应操作数。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_operands_from_int(std::int64_t value) -> PluralOperands;

/// @brief 由浮点数构造复数操作数（按最短十进制表示推导小数位，仅供不便携带原文本的调用方）。
/// @param value 数值（符号不参与分类）。
/// @return 对应操作数；非有限值（NaN/Inf）按零值处理。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_operands_from_double(double value) -> PluralOperands;

/// @brief 由十进制文本构造复数操作数（最精确路径；消息引擎的缺省选择）。
/// @param text 十进制文本（可带正负号与小数点；千位分组符等非十进制字符不支持）。
/// @return 对应操作数；无法解析时返回零值操作数（与旧版 `to_double` 回退零的口径一致）。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_operands_from_decimal(std::string_view text) -> PluralOperands;

/// @brief 按 CLDR 基数规则把操作数映射到复数类别（表驱动，CLDR 48 全语言）。
///
/// @param ops 复数操作数。
/// @param loc 区域设置；键取 `language`（未收录时回退 `Other`）。
/// @return 对应的 CLDR 复数类别。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_category(const PluralOperands &ops, const Locale &loc) -> PluralCategory;

/// @brief 按 CLDR 序数规则把操作数映射到复数类别（如 en `1st/2nd/3rd/4th` 的 one/two/few）。
/// @param ops 复数操作数。
/// @param loc 区域设置；键取 `language`（未收录时回退 `Other`）。
/// @return 对应的 CLDR 序数类别。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_category_ordinal(const PluralOperands &ops, const Locale &loc) -> PluralCategory;

/// @brief 按 CLDR 基数规则把数值 n 映射到复数类别（兼容便捷重载，内部走
/// `plural_operands_from_double` → `plural_category`；有原始十进制文本时优先用操作数重载）。
///
/// @param n   计数值（非负计数语义；负值取绝对值后判定）。
/// @param loc 区域设置。
/// @return 对应的 CLDR 复数类别。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] auto plural_category(double n, const Locale &loc) -> PluralCategory;

}  // namespace aurora
