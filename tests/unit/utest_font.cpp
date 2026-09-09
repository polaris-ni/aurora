/// 测试类型: unit
/// 目标单元: include/aurora/core/font.h
/// 测试说明: 覆盖 Font 的默认值契约、字段级相等/不等语义、自定义构造往返与 CSS 字重区间的可表示性

#include <string>

#include "aurora/core/font.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_font {

AURORA_TEST_CASE(default_is_sans_serif_14_regular) {
    // 默认值契约：sans-serif / 14pt / 常规字重 400。
    const aurora::Font font;
    AURORA_TEST_CHECK_STREQ(font.family, "sans-serif");
    AURORA_TEST_CHECK_EQ(font.size_pt, 14.0F);
    AURORA_TEST_CHECK_EQ(font.weight, 400);
}

AURORA_TEST_CASE(equality_holds_for_identical_fields) {
    const aurora::Font a;
    const aurora::Font b;
    AURORA_TEST_CHECK(a == b);
    AURORA_TEST_CHECK_FALSE(a != b);
}

AURORA_TEST_CASE(inequality_on_each_differing_field) {
    // 三个字段逐一验证：任一字段不同即不等（== / != 语义互斥一致）。
    const aurora::Font base;
    const aurora::Font other_family{.family = "serif", .size_pt = 14.0F, .weight = 400};
    const aurora::Font other_size{.family = "sans-serif", .size_pt = 15.0F, .weight = 400};
    const aurora::Font other_weight{.family = "sans-serif", .size_pt = 14.0F, .weight = 500};

    AURORA_TEST_CHECK(base != other_family);
    AURORA_TEST_CHECK(base != other_size);
    AURORA_TEST_CHECK(base != other_weight);
    AURORA_TEST_CHECK_FALSE(base == other_family);
    AURORA_TEST_CHECK_FALSE(base == other_size);
    AURORA_TEST_CHECK_FALSE(base == other_weight);
}

AURORA_TEST_CASE(custom_construction_roundtrips_fields) {
    const aurora::Font font{.family = "JetBrains Mono", .size_pt = 18.5F, .weight = 700};
    AURORA_TEST_CHECK_STREQ(font.family, "JetBrains Mono");
    AURORA_TEST_CHECK_NEAR(font.size_pt, 18.5, 1e-6);
    AURORA_TEST_CHECK_EQ(font.weight, 700);

    // 相同字面量构造出的两个对象相等（float 精确位型一致）。
    const aurora::Font same{.family = "JetBrains Mono", .size_pt = 18.5F, .weight = 700};
    AURORA_TEST_CHECK(font == same);
    AURORA_TEST_CHECK_FALSE(font != same);
}

AURORA_TEST_CASE(css_weight_range_is_representable) {
    // 100..900 步进 100 的字重全部可构造、可比较（遵循 CSS 字重约定）。
    constexpr int weights[] = {100, 200, 300, 400, 500, 600, 700, 800, 900};
    for (const int weight : weights) {
        const aurora::Font font{.family = "probe", .size_pt = 12.0F, .weight = weight};
        AURORA_TEST_CHECK_EQ(font.weight, weight);
    }
    // 字段独立：改字重不影响 family/size。
    const aurora::Font font{.family = "probe", .size_pt = 12.0F, .weight = 900};
    AURORA_TEST_CHECK_STREQ(font.family, "probe");
    AURORA_TEST_CHECK_EQ(font.size_pt, 12.0F);
}

AURORA_TEST_CASE(copy_preserves_value_independence) {
    // 聚合拷贝语义：副本改字段不得影响原对象。
    const aurora::Font original{.family = "serif", .size_pt = 16.0F, .weight = 600};
    aurora::Font copy = original;
    copy.family = "mono";
    copy.weight = 800;

    AURORA_TEST_CHECK_STREQ(original.family, "serif");
    AURORA_TEST_CHECK_EQ(original.weight, 600);
    AURORA_TEST_CHECK_STREQ(copy.family, "mono");
    AURORA_TEST_CHECK_EQ(copy.weight, 800);
    AURORA_TEST_CHECK(original != copy);
}

}  // namespace aurora::test_cases::utest_font
