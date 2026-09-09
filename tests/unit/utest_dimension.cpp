/// 测试类型: unit
/// 目标单元: include/aurora/core/dimension.h
/// 测试说明: 覆盖 Length 工厂（px/dp/percent/fill/auto_length）的 kind/value 契约、to_string 渲染、_dp/_px 字面量与裸标量隐式转换禁令

#include <string>
#include <type_traits>

#include "aurora/core/dimension.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dimension {

namespace au = aurora;
namespace m = aurora::testing::matchers;

/// @brief 默认 Length 为 wrap_content、值为 0。
AURORA_TEST_CASE(default_length_is_wrap_content) {
    constexpr aurora::Length len{};
    static_assert(len.kind == aurora::LengthKind::WrapContent);
    static_assert(len.value == 0.0F);
    AURORA_TEST_CHECK_EQ(len.kind, aurora::LengthKind::WrapContent);
    AURORA_TEST_CHECK_EQ(len.value, 0.0F);
}

/// @brief 工厂与 kind/value 的映射契约（全部 constexpr 可用）。
AURORA_TEST_CASE(factories_produce_expected_kinds_and_values) {
    static_assert(au::px(120.0F).kind == aurora::LengthKind::Fixed);
    static_assert(au::px(120.0F).value == 120.0F);
    static_assert(au::dp(24.0F).kind == aurora::LengthKind::Fixed);
    static_assert(au::percent(0.5F).kind == aurora::LengthKind::Fraction);
    static_assert(au::percent(0.5F).value == 0.5F);
    static_assert(au::fill().kind == aurora::LengthKind::Expand);
    static_assert(au::fill().value == 0.0F);
    static_assert(au::auto_length().kind == aurora::LengthKind::WrapContent);

    AURORA_TEST_CHECK_EQ(au::percent(0.8F).kind, aurora::LengthKind::Fraction);
    AURORA_TEST_CHECK_EQ(au::percent(0.8F).value, 0.8F);
    AURORA_TEST_CHECK_EQ(au::auto_length().kind, aurora::LengthKind::WrapContent);
}

/// @brief dp 与 px 当前语义等价（同为逻辑像素的 Fixed 意图）。
AURORA_TEST_CASE(dp_matches_px_semantics) {
    constexpr auto as_dp = au::dp(120.0F);
    constexpr auto as_px = au::px(120.0F);
    static_assert(as_dp.kind == as_px.kind);
    static_assert(as_dp.value == as_px.value);
    AURORA_TEST_CHECK_EQ(as_dp.kind, aurora::LengthKind::Fixed);
    AURORA_TEST_CHECK_EQ(as_dp.value, as_px.value);
}

/// @brief to_string 按意图类型渲染为可读字符串（调试快照用）。
AURORA_TEST_CASE(to_string_renders_each_kind) {
    AURORA_TEST_CHECK_STREQ(au::to_string(au::auto_length()), "auto");
    AURORA_TEST_CHECK_STREQ(au::to_string(au::fill()), "fill");
    AURORA_TEST_CHECK_THAT(au::to_string(au::px(120.0F)), m::starts_with("px("));
    AURORA_TEST_CHECK_THAT(au::to_string(au::px(120.0F)), m::has_substr("120"));
    AURORA_TEST_CHECK_THAT(au::to_string(au::percent(0.8F)), m::starts_with("percent("));
    AURORA_TEST_CHECK_THAT(au::to_string(au::percent(0.8F)), m::has_substr("0.8"));
}

/// @brief 合法边界值（fixed(0) / ratio(0) / ratio(1)）可用且 constexpr。
/// @note 负值与 >1 的取值属前置条件违例：AURORA_ASSERT 为 debug-only（NDEBUG 下裁切），
///       Debug 构建下触发即 abort，故不在单元测试中触探该路径。
AURORA_TEST_CASE(boundary_values_are_valid) {
    static_assert(au::Length::fixed(0.0F).kind == aurora::LengthKind::Fixed);
    static_assert(au::Length::fixed(0.0F).value == 0.0F);
    static_assert(au::Length::ratio(0.0F).value == 0.0F);
    static_assert(au::Length::ratio(1.0F).kind == aurora::LengthKind::Fraction);
    static_assert(au::Length::ratio(1.0F).value == 1.0F);
    static_assert(au::Length::wrap().kind == aurora::LengthKind::WrapContent);
    static_assert(au::Length::expand().kind == aurora::LengthKind::Expand);
    AURORA_TEST_CHECK_EQ(au::Length::fixed(0.0F).value, 0.0F);
    AURORA_TEST_CHECK_EQ(au::Length::ratio(1.0F).value, 1.0F);
}

/// @brief _dp/_px 字面量与工厂等价（整型与 long double 两种重载）。
AURORA_TEST_CASE(dp_px_literals_match_factories) {
    using aurora::literals::operator""_dp;  // using-declaration：仅引入具名字面量（库约定：TU 内显式引入）
    using aurora::literals::operator""_px;

    static_assert((120_dp).kind == aurora::LengthKind::Fixed); // NOLINT(*-redundant-parentheses)
    static_assert((120_dp).value == 120.0F); // NOLINT(*-redundant-parentheses)
    AURORA_TEST_CHECK((120_dp).kind == au::px(120.0F).kind);
    AURORA_TEST_CHECK_EQ((120_dp).value, au::px(120.0F).value);
    AURORA_TEST_CHECK_EQ((8_px).value, 8.0F);
    AURORA_TEST_CHECK_NEAR((2.5_px).value, 2.5F, 1e-6F);  // long double 重载
}

/// @brief 编译期契约：裸 int/float/double 均不可隐式转为 Length（本头文件的核心设计目标）。
AURORA_TEST_CASE(length_rejects_raw_scalar_conversion) {
    static_assert(!std::is_convertible_v<int, aurora::Length>);
    static_assert(!std::is_convertible_v<unsigned int, aurora::Length>);
    static_assert(!std::is_convertible_v<float, aurora::Length>);
    static_assert(!std::is_convertible_v<double, aurora::Length>);
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_dimension
