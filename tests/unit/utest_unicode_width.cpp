/// 测试类型: unit
/// 目标单元: include/aurora/core/unicode_width.h
/// 测试说明: 覆盖「零宽优先于 East Asian Width」的判定次序、W/F 双宽、Ambiguous 随口径取值、中性类单宽、
/// 区间表端点边界，以及全码点扫描下返回值只落在 {0,1,2}

#include <cstdint>

#include "aurora/core/unicode_width.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_unicode_width {

AURORA_TEST_CASE(zero_width_precedes_east_asian_width) {
    // 软连字符同时是 Cf 与 Ambiguous：次序颠倒则 Wide 口径下它占 2 格，把所并入的基础格挤成半格。
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u00AD', AmbiguousWidthMode::Narrow), 0U);
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u00AD', AmbiguousWidthMode::Wide), 0U);
    // 组合区段 0300..036F、变体选择符 FE00..FE0F、标签区段 E0020..E007F 在 UCD 里同样带 Ambiguous 标记。
    for (const char32_t code_point :
         {U'\u0300', U'\u036F', U'\uFE00', U'\uFE0F', U'\u200B', U'\U000E0100', U'\U000E01EF', U'\u0301'}) {
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Narrow), 0U);
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Wide), 0U);
    }
}

AURORA_TEST_CASE(wide_and_fullwidth_classes_occupy_two_cells) {
    // W 类：CJK 表意、韩文音节、emoji、朝鲜文初声。
    for (const char32_t code_point : {U'\u4E00', U'\uAC00', U'\U0001F600', U'\u1100'}) {
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Narrow), 2U);
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Wide), 2U);
    }
    // F 类：全角空格与全角拉丁字母。
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u3000', AmbiguousWidthMode::Narrow), 2U);
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\uFF21', AmbiguousWidthMode::Wide), 2U);
}

AURORA_TEST_CASE(ambiguous_class_follows_the_mode) {
    // 度符号、右箭头、箱线字符、拉丁重音字母：两种口径都是既成事实，框架不替调用方选边。
    for (const char32_t code_point : {U'\u00B0', U'\u2192', U'\u2500', U'\u00E9'}) {
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Narrow), 1U);
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Wide), 2U);
    }
}

AURORA_TEST_CASE(neutral_and_narrow_classes_occupy_one_cell) {
    // Na/N 类，含 Mc（占格组合符，非零宽）与 U+1160（朝鲜文中声，紧邻双宽的 U+115F 却是单宽）。
    for (const char32_t code_point : {U'A', U'~', U'\u00A9', U'\u0903', U'\u1160', U'\u2028'}) {
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Narrow), 1U);
        AURORA_TEST_CHECK_EQ(unicode_cell_width(code_point, AmbiguousWidthMode::Wide), 1U);
    }
}

AURORA_TEST_CASE(interval_table_boundaries) {
    // 区间按相邻同类合并，端点判定的对错只会在边界上暴露：每个断言都取「表内末位 / 表外首位」。
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u115F', AmbiguousWidthMode::Narrow), 2U);  // W 段末
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\uFF60', AmbiguousWidthMode::Narrow), 2U);  // F 段末
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\uFF61', AmbiguousWidthMode::Narrow), 1U);  // 半角起
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u3247', AmbiguousWidthMode::Wide), 2U);  // W 段末
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u3248', AmbiguousWidthMode::Wide), 2U);  // A 段首（Wide 口径）
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u3248', AmbiguousWidthMode::Narrow), 1U);  // A 段首（Narrow 口径）
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u036F', AmbiguousWidthMode::Wide), 0U);  // 零宽段末
    AURORA_TEST_CHECK_EQ(unicode_cell_width(U'\u0370', AmbiguousWidthMode::Wide), 1U);  // 紧随其后的单宽
}

AURORA_TEST_CASE(return_value_is_always_zero_one_or_two) {
    // 全码点扫描：代理区不是合法标量值，跳过；其余一律只可能返回 0、1 或 2。
    for (char32_t code_point = 0U; code_point <= 0x10FFFFU; ++code_point) {
        if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
            continue;
        }
        const auto narrow = unicode_cell_width(code_point, AmbiguousWidthMode::Narrow);
        const auto wide = unicode_cell_width(code_point, AmbiguousWidthMode::Wide);
        AURORA_TEST_CHECK(narrow <= 2U);
        AURORA_TEST_CHECK(wide <= 2U);
        // 口径只可能把单宽抬成双宽，绝不会改变零宽或压缩双宽。
        AURORA_TEST_CHECK(wide >= narrow);
    }
}

}  // namespace aurora::test_cases::utest_unicode_width
