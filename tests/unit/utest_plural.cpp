/// 测试类型: unit
/// 目标单元: include/aurora/i18n/plural.h
/// 测试说明: 复数操作数推导（整数/浮点/十进制文本）、CLDR 表驱动基数与序数判定、
///           CLDR @integer/@decimal 样例全量回归（plural_rules_samples.gen.inc）

#include <string>

#include "aurora/i18n/plural.h"
#include "framework/aurora_test.h"
#include "plural_rules_samples.gen.inc"  // CLDR 48 导出样例（生成件；与 plural_rules.gen.cpp 同源同表）

namespace aurora::test_cases::utest_plural {

AURORA_TEST_CASE(operands_from_int_have_no_fraction) {
    // 整数：i 即数值本身，小数位全零。
    const auto ops = aurora::plural_operands_from_int(1234);
    AURORA_TEST_CHECK_EQ(ops.n, 1234.0);
    AURORA_TEST_CHECK_EQ(ops.i, 1234LL);
    AURORA_TEST_CHECK_EQ(ops.v, 0);
    AURORA_TEST_CHECK_EQ(ops.w, 0);
    AURORA_TEST_CHECK_EQ(ops.f, 0LL);
    AURORA_TEST_CHECK_EQ(ops.t, 0LL);
}

AURORA_TEST_CASE(operands_from_decimal_split_digits) {
    // 十进制文本：v/f 含尾随零，w/t 去尾随零（TR35 操作数口径）。
    const auto ops = aurora::plural_operands_from_decimal("1.20");
    AURORA_TEST_CHECK_EQ(ops.v, 2);
    AURORA_TEST_CHECK_EQ(ops.w, 1);
    AURORA_TEST_CHECK_EQ(ops.f, 20LL);
    AURORA_TEST_CHECK_EQ(ops.t, 2LL);
    const auto whole = aurora::plural_operands_from_decimal("1.00");
    AURORA_TEST_CHECK_EQ(whole.v, 2);
    AURORA_TEST_CHECK_EQ(whole.w, 0);
    AURORA_TEST_CHECK_EQ(whole.t, 0LL);
}

AURORA_TEST_CASE(operands_from_invalid_text_fall_back_to_zero) {
    // 非十进制文本（千位分组 / 字母 / 空）→ 零值操作数，与旧 to_double 回退口径一致。
    const auto bad = aurora::plural_operands_from_decimal("1,234");
    AURORA_TEST_CHECK_EQ(bad.n, 0.0);
    AURORA_TEST_CHECK_EQ(aurora::plural_operands_from_decimal("").v, 0);
}

AURORA_TEST_CASE(cardinal_rules_cldr_table) {
    // 表驱动基数判定：覆盖六类别与旧手写表同口径的语言行为。
    const auto cat = [](const char *lang, double n) {
        return aurora::plural_category(n, aurora::Locale{.language = lang});
    };
    AURORA_TEST_CHECK(cat("en", 1) == aurora::PluralCategory::One);
    AURORA_TEST_CHECK(cat("en", 2) == aurora::PluralCategory::Other);
    AURORA_TEST_CHECK(cat("ru", 21) == aurora::PluralCategory::One);
    AURORA_TEST_CHECK(cat("ru", 11) == aurora::PluralCategory::Many);
    AURORA_TEST_CHECK(cat("ru", 3) == aurora::PluralCategory::Few);
    AURORA_TEST_CHECK(cat("ar", 0) == aurora::PluralCategory::Zero);
    AURORA_TEST_CHECK(cat("ar", 2) == aurora::PluralCategory::Two);
    AURORA_TEST_CHECK(cat("ar", 103) == aurora::PluralCategory::Few);
    AURORA_TEST_CHECK(cat("fr", 0) == aurora::PluralCategory::One);
    AURORA_TEST_CHECK(cat("fr", 1000000) == aurora::PluralCategory::Many);
    AURORA_TEST_CHECK(cat("zh", 5) == aurora::PluralCategory::Other);
}

AURORA_TEST_CASE(unknown_language_falls_back_to_other) {
    // 未收录语言 → Other（表驱动口径；旧手写表回退 en 规则， breaking 差异记 CHANGELOG）。
    AURORA_TEST_CHECK(aurora::plural_category(1, aurora::Locale{.language = "xx"}) == aurora::PluralCategory::Other);
}

AURORA_TEST_CASE(ordinal_rules_cover_english_suffixes) {
    // 序数表：en 1st/2nd/3rd/4th/11th。
    const aurora::Locale en{.language = "en"};
    using O = aurora::PluralCategory;
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(1), en) == O::One);
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(2), en) == O::Two);
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(3), en) == O::Few);
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(4), en) == O::Other);
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(11), en) == O::Other);
    AURORA_TEST_CHECK(aurora::plural_category_ordinal(aurora::plural_operands_from_int(111), en) == O::Other);
}

AURORA_TEST_CASE(cldr_sample_fixtures_roundtrip_through_table) {
    // CLDR 样例全量回归：每条 @integer/@decimal 样例按十进制串走操作数路径，与判定表逐条对齐。
    int checked = 0;
    for (const auto &sample : AURORA_PLURAL_RULE_SAMPLES) {
        const aurora::Locale loc{.language = std::string(sample.language)};
        const auto ops = aurora::plural_operands_from_decimal(sample.number);
        const auto got = sample.ordinal ? aurora::plural_category_ordinal(ops, loc) : aurora::plural_category(ops, loc);
        AURORA_TEST_CHECK_EQ(got, sample.expected);
        ++checked;
    }
    AURORA_TEST_REQUIRE(checked > 1000);  // 样例集须实质非空（CLDR 48 导出口径）
}

}  // namespace aurora::test_cases::utest_plural
