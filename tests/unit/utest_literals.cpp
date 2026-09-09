/// 测试类型: unit
/// 目标单元: include/aurora/core/literals.h
/// 测试说明: 覆盖字面量统一入口聚合五类 UDL（_rgb/_rgba/_dp/_px/_ms）与各领域工厂的一致性、通道字节序与隐式转换禁令

#include <type_traits>

#include "aurora/core/literals.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_literals {

namespace au = aurora;

/// @brief 单一入口头聚合全部字面量族：同一用例内五类 UDL 均可用（验证文档声明）。
AURORA_TEST_CASE(single_entry_header_aggregates_all_families) {
    using aurora::literals::operator""_rgb;  // using-declaration：仅引入具名字面量（库约定：TU 内显式引入）
    using aurora::literals::operator""_rgba;
    using aurora::literals::operator""_dp;
    using aurora::literals::operator""_px;
    using aurora::literals::operator""_ms;

    AURORA_TEST_CHECK((0xFF0000_rgb) == aurora::Color::red());
    AURORA_TEST_CHECK_EQ((0x0000FFFF_rgba).a, 255);
    AURORA_TEST_CHECK_EQ((120_dp).kind, aurora::LengthKind::Fixed);
    AURORA_TEST_CHECK_EQ((8_px).value, 8.0F);
    AURORA_TEST_CHECK((250_ms) == aurora::Duration::from_ms(250.0));
}

/// @brief 颜色字面量与静态工厂一致，constexpr 可用于编译期断言。
AURORA_TEST_CASE(color_literals_agree_with_factories) {
    using aurora::literals::operator""_rgb;
    using aurora::literals::operator""_rgba;

    static_assert(0xFF0000_rgb == aurora::Color::red());
    static_assert(0x0000FFFF_rgba == aurora::Color::blue());
    AURORA_TEST_CHECK((0x00FF00_rgb) == aurora::Color{0, 255, 0});
    AURORA_TEST_CHECK((0xFF0000_rgb) == aurora::Color::red());
    AURORA_TEST_CHECK((0x0000FFFF_rgba) == aurora::Color::blue());
}

/// @brief 尺寸字面量与 px/dp 工厂逐字段一致（整型与 long double 两种重载）。
AURORA_TEST_CASE(dimension_literals_agree_with_factories) {
    using aurora::literals::operator""_dp;
    using aurora::literals::operator""_px;

    static_assert((120_dp).kind == aurora::LengthKind::Fixed);  // NOLINT(*-redundant-parentheses)
    static_assert((120_dp).value == 120.0F);  // NOLINT(*-redundant-parentheses)
    AURORA_TEST_CHECK_EQ((120_dp).value, au::px(120.0F).value);
    AURORA_TEST_CHECK_EQ((120_dp).kind, au::dp(120.0F).kind);
    AURORA_TEST_CHECK_EQ((8_px).value, 8.0F);
    AURORA_TEST_CHECK_NEAR((2.5_px).value, au::px(2.5F).value, 1e-6F);
}

/// @brief 时长字面量与 from_ms 一致，250ms 精确映射 0.25s。
AURORA_TEST_CASE(duration_literal_agrees_with_factory) {
    using aurora::literals::operator""_ms;

    static_assert(250_ms == aurora::Duration{0.25});
    AURORA_TEST_CHECK((250_ms) == aurora::Duration::from_ms(250.0));
    AURORA_TEST_CHECK_EQ((0_ms).seconds, 0.0);
    AURORA_TEST_CHECK_NEAR((12.5_ms).seconds, aurora::Duration::from_ms(12.5).seconds, 1e-12);
}

/// @brief _rgba 高字节在前（RRGGBBAA）；_rgb 无 alpha 段时默认不透明。
AURORA_TEST_CASE(rgba_literal_byte_order_is_high_first) {
    using aurora::literals::operator""_rgb;
    using aurora::literals::operator""_rgba;

    constexpr auto full = 0xAABBCCDD_rgba;
    static_assert(full.r == 0xAA && full.g == 0xBB && full.b == 0xCC && full.a == 0xDD);
    AURORA_TEST_CHECK_EQ(full.r, 0xAA);
    AURORA_TEST_CHECK_EQ(full.a, 0xDD);
    AURORA_TEST_CHECK_EQ((0x123456_rgb).a, 255);
}

/// @brief 编译期契约：三个类型族的隐式标量转换禁令在统一入口下依然成立。
AURORA_TEST_CASE(forbidden_implicit_conversions_still_hold) {
    static_assert(!std::is_convertible_v<int, aurora::Length>);
    static_assert(!std::is_convertible_v<float, aurora::Length>);
    static_assert(!std::is_convertible_v<int, aurora::Color>);
    static_assert(!std::is_convertible_v<double, aurora::Duration>);
    static_assert(std::is_constructible_v<aurora::Duration, double>);  // explicit 构造仍可用
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_literals
