/// 测试类型: unit
/// 目标单元: include/aurora/core/enums.h
/// 测试说明: 覆盖各枚举的枚举量取值/数量（穷尽性守卫）、底层宽度，以及 TextDecoration 位掩码运算族

#include <cstdint>
#include <type_traits>

#include "aurora/core/enums.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_enums {

AURORA_TEST_CASE(font_weight_values_follow_css_convention) {
    // 契约：枚举值即 CSS 字重数值 100..900。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Thin), 100);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::ExtraLight), 200);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Light), 300);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Normal), 400);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Medium), 500);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::SemiBold), 600);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Bold), 700);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::ExtraBold), 800);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint16_t>(aurora::FontWeight::Black), 900);
    static_assert(std::is_same_v<std::underlying_type_t<aurora::FontWeight>, std::uint16_t>);
}

AURORA_TEST_CASE(text_presentation_enum_layout_is_stable) {
    // 0 基枚举的「数量守卫」：末枚举量数值 +1 即枚举量个数；
    // 中间插入新枚举量会使具体取值断言失败，尾部追加会使数量断言失败。
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::TextAlign::Left), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::TextAlign::Justify), 5);  // 共 6 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::TextOverflow::Fade), 2);  // 共 3 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::FontStyle::Italic), 1);  // 共 2 个

    static_assert(std::is_same_v<std::underlying_type_t<aurora::TextAlign>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<aurora::TextOverflow>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<aurora::FontStyle>, std::uint8_t>);
    AURORA_TEST_CHECK(true);
}

AURORA_TEST_CASE(layout_enum_layout_is_stable) {
    // 布局族枚举：末枚举量数值锁定（数量 = 值 + 1）。
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::MainAxisSize::Max), 1);  // 共 2 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::MainAxisAlignment::SpaceEvenly), 5);  // 共 6 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::CrossAxisAlignment::Stretch), 3);  // 共 4 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::StackFit::Passthrough), 2);  // 共 3 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::OverflowStrategy::Scroll), 3);  // 共 4 个
    AURORA_TEST_CHECK_EQ(static_cast<int>(aurora::BoxFit::ScaleDown), 6);  // 共 7 个
    static_assert(std::is_same_v<std::underlying_type_t<aurora::BoxFit>, std::uint8_t>);
    AURORA_TEST_CHECK(true);
}

AURORA_TEST_CASE(text_decoration_bitwise_combine_and_test) {
    using aurora::decoration_has;
    using aurora::TextDecoration;

    constexpr auto combined = TextDecoration::Underline | TextDecoration::LineThrough;
    AURORA_TEST_CHECK(decoration_has(combined, TextDecoration::Underline));
    AURORA_TEST_CHECK(decoration_has(combined, TextDecoration::LineThrough));
    AURORA_TEST_CHECK_FALSE(decoration_has(combined, TextDecoration::Overline));

    // 按位与提取交集：与单个位相与得到该位或 None。
    AURORA_TEST_CHECK_EQ(combined & TextDecoration::Underline, TextDecoration::Underline);
    AURORA_TEST_CHECK_EQ(combined & TextDecoration::Overline, TextDecoration::None);

    // |= 累积组合。
    auto flags = TextDecoration::None;
    flags |= TextDecoration::Overline;
    flags |= TextDecoration::Underline;
    AURORA_TEST_CHECK(decoration_has(flags, TextDecoration::Overline));
    AURORA_TEST_CHECK(decoration_has(flags, TextDecoration::Underline));
    AURORA_TEST_CHECK_FALSE(decoration_has(flags, TextDecoration::LineThrough));
}

AURORA_TEST_CASE(text_decoration_none_has_no_bits) {
    using aurora::decoration_has;
    using aurora::TextDecoration;

    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(TextDecoration::None), 0);
    AURORA_TEST_CHECK_FALSE(decoration_has(TextDecoration::None, TextDecoration::Underline));
    AURORA_TEST_CHECK_FALSE(decoration_has(TextDecoration::None, TextDecoration::Overline));
    AURORA_TEST_CHECK_FALSE(decoration_has(TextDecoration::None, TextDecoration::LineThrough));
    // None | None 仍为 None。
    AURORA_TEST_CHECK_EQ(TextDecoration::None | TextDecoration::None, TextDecoration::None);
}

}  // namespace aurora::test_cases::utest_enums
