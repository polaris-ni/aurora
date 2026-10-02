/// 测试类型: unit
/// 目标单元: include/aurora/core/unicode_width.h
/// 测试说明: 覆盖「零宽优先于 East Asian Width」的判定次序、W/F 双宽、Ambiguous 随口径取值、中性类单宽、
/// 区间表端点边界，全码点扫描下返回值只落在 {0,1,2}，以及单宽短路与逐表二分在全码点上的逐点等价

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

// 全码点穷举等值守门：函数内那条「小于单宽下界直接返回 1」的短路必须与短路前的逐表二分**逐码点等价**，
// 而不是「看起来差不多」。判据取两组与实现无关的常数——0/1/2 的分布计数 + 覆盖全部码点的 FNV-1a 64 位摘要，
// 二者任一不符即转红：只钉计数会漏掉「计数恰好不变但逐点错位」，只钉摘要在分布漂移时不易定位，故两者都断言。
//
// 常数如何复现（务必在改动本函数**之前**取，否则等于拿新实现的自洽当正确性）：
//   1) 用改动前的 src/aurora/core/unicode_width.cpp 与本文件同形的遍历口径跑一遍全码点；
//   2) 每种口径下累加 0/1/2 的计数，并按码点升序把返回值并入 FNV-1a 64：
//      种子 = 偏移基 14695981039346656037 XOR static_cast<std::uint64_t>(mode)，每步 hash ^= 返回值; hash *=
//      1099511628211；
//   3) 两侧口径各自记下计数三元组与摘要，写进下面的常量。
// 另用一份独立实现（按表重写的 Python 脚本同样口径）交叉复算过，两法一致，排除「摘要只是复述 C++ 自己的错」。
//
// 遍历含代理区段（D800..DFFF）：既有实现对这些值同样返回确定结果，短路不得改变它，故一并纳入而不跳过。
AURORA_TEST_CASE(exhaustive_scan_matches_the_table_driven_reference) {
    constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    // 实测常数（变更前的树上取得，口径见上）。用结构体而非 std::pair：数组形参会退化成指针，
    // 类模板实参推导会失败。
    struct Expectation {
        AmbiguousWidthMode mode;
        std::uint64_t zero;
        std::uint64_t one;
        std::uint64_t two;
        std::uint64_t digest;
    };
    constexpr Expectation narrow{.mode = AmbiguousWidthMode::Narrow,
                                 .zero = 2273ULL,
                                 .one = 927980ULL,
                                 .two = 183859ULL,
                                 .digest = 5890205287482552179ULL};
    constexpr Expectation wide{.mode = AmbiguousWidthMode::Wide,
                               .zero = 2273ULL,
                               .one = 789610ULL,
                               .two = 322229ULL,
                               .digest = 3203393021350612880ULL};

    for (const auto &expected : {narrow, wide}) {
        std::uint64_t zero = 0ULL;
        std::uint64_t one = 0ULL;
        std::uint64_t two = 0ULL;
        std::uint64_t digest = fnv_offset_basis ^ static_cast<std::uint64_t>(expected.mode);
        for (char32_t code_point = 0U; code_point <= 0x10FFFFU; ++code_point) {
            const auto width = unicode_cell_width(code_point, expected.mode);
            AURORA_TEST_REQUIRE(width <= 2U);
            // 三个分支各自累加，不用 counts[width] 下标：下标形式会被 clang-tidy 的定长数组越界检查拦下。
            if (width == 0U) {
                ++zero;
            } else if (width == 1U) {
                ++one;
            } else {
                ++two;
            }
            digest ^= static_cast<std::uint64_t>(width);
            digest *= fnv_prime;
        }
        AURORA_TEST_CHECK_EQ(zero, expected.zero);
        AURORA_TEST_CHECK_EQ(one, expected.one);
        AURORA_TEST_CHECK_EQ(two, expected.two);
        AURORA_TEST_CHECK_EQ(digest, expected.digest);
    }
}

}  // namespace aurora::test_cases::utest_unicode_width
