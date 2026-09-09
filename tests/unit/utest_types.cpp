/// 测试类型: unit
/// 目标单元: include/aurora/core/types.h
/// 测试说明: 覆盖 Point/Size/Rect/EdgeInsets/Constraints/Length
/// 的构造默认值、几何运算、命中与相交边界、约束钳制与强类型转换禁令

#include <cstdint>
#include <limits>
#include <type_traits>

#include "aurora/core/types.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_types {

namespace au = aurora;

/// @brief Point 默认 (0,0)，加减为逐分量运算。
AURORA_TEST_CASE(point_default_and_componentwise_arithmetic) {
    constexpr aurora::Point p{};
    static_assert(p.x == 0.0F && p.y == 0.0F);
    constexpr aurora::Point a{.x = 1.0F, .y = 2.0F};
    constexpr aurora::Point b{.x = 4.0F, .y = 6.0F};
    constexpr auto sum = a + b;
    constexpr auto diff = a - b;
    static_assert(sum.x == 5.0F && sum.y == 8.0F);
    static_assert(diff.x == -3.0F && diff.y == -4.0F);
    AURORA_TEST_CHECK_NEAR(sum.x, 5.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(diff.y, -4.0F, 1e-6F);
}

/// @brief Size 支持缩放与逐分量加减。
AURORA_TEST_CASE(size_arithmetic_and_scaling) {
    constexpr aurora::Size s{.width = 10.0F, .height = 20.0F};
    constexpr auto scaled = s * 0.5F;
    constexpr auto sum = s + aurora::Size{.width = 1.0F, .height = 2.0F};
    constexpr auto diff = s - aurora::Size{.width = 1.0F, .height = 2.0F};
    static_assert(scaled.width == 5.0F && scaled.height == 10.0F);
    static_assert(sum.width == 11.0F && sum.height == 22.0F);
    static_assert(diff.width == 9.0F && diff.height == 18.0F);
    AURORA_TEST_CHECK_NEAR(scaled.height, 10.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(diff.width, 9.0F, 1e-6F);
}

/// @brief Size::infinity() 双轴无限；is_finite 按「任一轴为 inf 即非有限」判定。
AURORA_TEST_CASE(size_infinity_and_finiteness) {
    constexpr auto unbounded = aurora::Size::infinity();
    static_assert(!unbounded.is_finite());
    constexpr aurora::Size finite{.width = 3.0F, .height = 4.0F};
    static_assert(finite.is_finite());
    AURORA_TEST_CHECK_FALSE(unbounded.is_finite());
    AURORA_TEST_CHECK_TRUE(finite.is_finite());
    // 仅一轴无限（如 wrap_content 的宽度约束）同样判为非有限。
    constexpr aurora::Size half_infinite{.width = 3.0F, .height = std::numeric_limits<float>::infinity()};
    AURORA_TEST_CHECK_FALSE(half_infinite.is_finite());
}

/// @brief Rect 的 right/bottom 推导与 contains 命中判定（边界含端点）。
AURORA_TEST_CASE(rect_boundaries_and_contains_hit_testing) {
    constexpr aurora::Rect rect{.origin = {.x = 0.0F, .y = 0.0F}, .size = {.width = 10.0F, .height = 10.0F}};
    AURORA_TEST_CHECK_NEAR(rect.right(), 10.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(rect.bottom(), 10.0F, 1e-6F);
    AURORA_TEST_CHECK(rect.contains({.x = 5.0F, .y = 5.0F}));  // 内部
    AURORA_TEST_CHECK(rect.contains({.x = 0.0F, .y = 0.0F}));  // 左上角（含端点）
    AURORA_TEST_CHECK(rect.contains({.x = 10.0F, .y = 10.0F}));  // 右下角（含端点）
    AURORA_TEST_CHECK(rect.contains({.x = 10.0F, .y = 5.0F}));  // 右边界
    AURORA_TEST_CHECK_FALSE(rect.contains({.x = 10.0F + 0.001F, .y = 5.0F}));  // 右侧之外
    AURORA_TEST_CHECK_FALSE(rect.contains({.x = -0.001F, .y = 5.0F}));  // 左侧之外
    AURORA_TEST_CHECK_FALSE(rect.contains({.x = 5.0F, .y = 10.0F + 0.001F}));  // 下方之外
}

/// @brief intersects 为保守相交：重叠/包含为真，边缘恰好相接为假（严格不等式）。
AURORA_TEST_CASE(rect_intersects_excludes_edge_touching) {
    constexpr aurora::Rect base{.origin = {.x = 0.0F, .y = 0.0F}, .size = {.width = 10.0F, .height = 10.0F}};
    AURORA_TEST_CHECK(base.intersects(
        aurora::Rect{.origin = {.x = 5.0F, .y = 5.0F}, .size = {.width = 10.0F, .height = 10.0F}}));  // 部分重叠
    AURORA_TEST_CHECK(base.intersects(
        aurora::Rect{.origin = {.x = 1.0F, .y = 1.0F}, .size = {.width = 2.0F, .height = 2.0F}}));  // 完全包含于内
    AURORA_TEST_CHECK(base.intersects(
        aurora::Rect{.origin = {.x = 0.0F, .y = 0.0F}, .size = {.width = 10.0F, .height = 10.0F}}));  // 恒等
    AURORA_TEST_CHECK_FALSE(base.intersects(
        aurora::Rect{.origin = {.x = 10.0F, .y = 0.0F}, .size = {.width = 5.0F, .height = 5.0F}}));  // 左边缘相接
    AURORA_TEST_CHECK_FALSE(base.intersects(
        aurora::Rect{.origin = {.x = 0.0F, .y = 10.0F}, .size = {.width = 5.0F, .height = 5.0F}}));  // 上边缘相接
    AURORA_TEST_CHECK_FALSE(base.intersects(
        aurora::Rect{.origin = {.x = 20.0F, .y = 20.0F}, .size = {.width = 5.0F, .height = 5.0F}}));  // 完全分离
}

/// @brief Rect 相等比较为逐字段（Display List 缓存命中判定契约）。
AURORA_TEST_CASE(rect_equality_is_field_wise) {
    constexpr aurora::Rect base{.origin = {.x = 1.0F, .y = 2.0F}, .size = {.width = 3.0F, .height = 4.0F}};
    AURORA_TEST_CHECK_EQ(base, aurora::Rect{{.x = 1.0F, .y = 2.0F}, {.width = 3.0F, .height = 4.0F}});
    AURORA_TEST_CHECK_NE(base, aurora::Rect{{.x = 9.0F, .y = 2.0F}, {.width = 3.0F, .height = 4.0F}});  // origin.x 不同
    AURORA_TEST_CHECK(base !=
                      aurora::Rect{{.x = 1.0F, .y = 2.0F}, {.width = 3.0F, .height = 9.0F}});  // size.height 不同
    AURORA_TEST_CHECK_FALSE(base != aurora::Rect{{.x = 1.0F, .y = 2.0F}, {.width = 3.0F, .height = 4.0F}});
}

/// @brief EdgeInsets 的水平/垂直求和（库中未标 constexpr，走运行期断言）。
AURORA_TEST_CASE(edge_insets_axis_sums) {
    constexpr aurora::EdgeInsets insets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F};
    constexpr aurora::EdgeInsets zero{};
    AURORA_TEST_CHECK_NEAR(insets.horizontal(), 6.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(insets.vertical(), 8.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(zero.horizontal(), 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(zero.vertical(), 0.0F, 1e-6F);
}

/// @brief Constraints::constrain 把尺寸夹入 [min, max]；默认 max 无限即只托底（库中未标 constexpr）。
AURORA_TEST_CASE(constraints_clamp_into_min_max) {
    constexpr aurora::Constraints unbounded{};
    const auto below = unbounded.constrain(aurora::Size{.width = -3.0F, .height = -3.0F});
    AURORA_TEST_CHECK_NEAR(below.width, 0.0F, 1e-6F);  // 默认 min 为 0
    AURORA_TEST_CHECK_NEAR(below.height, 0.0F, 1e-6F);

    constexpr aurora::Constraints bounded{.min = aurora::Size{.width = 10.0F, .height = 10.0F},
                                          .max = aurora::Size{.width = 100.0F, .height = 100.0F}};
    const auto mixed = bounded.constrain(aurora::Size{.width = 50.0F, .height = 200.0F});
    AURORA_TEST_CHECK_NEAR(mixed.width, 50.0F, 1e-6F);  // 区间内保持
    AURORA_TEST_CHECK_NEAR(mixed.height, 100.0F, 1e-6F);  // 超上界被夹
    const auto low = bounded.constrain(aurora::Size{.width = 1.0F, .height = 1.0F});
    AURORA_TEST_CHECK_NEAR(low.width, 10.0F, 1e-6F);  // 低于下界被托
}

/// @brief Constraints 相等比较为逐字段（布局缓存键契约）。
AURORA_TEST_CASE(constraints_equality_is_field_wise) {
    constexpr aurora::Constraints a{.min = aurora::Size{.width = 10.0F, .height = 10.0F},
                                    .max = aurora::Size{.width = 100.0F, .height = 100.0F}};
    constexpr aurora::Constraints b{.min = aurora::Size{.width = 10.0F, .height = 10.0F},
                                    .max = aurora::Size{.width = 100.0F, .height = 100.0F}};
    constexpr aurora::Constraints c{.min = aurora::Size{.width = 0.0F, .height = 0.0F},
                                    .max = aurora::Size{.width = 100.0F, .height = 100.0F}};
    AURORA_TEST_CHECK(a == b);
    AURORA_TEST_CHECK(a != c);
    AURORA_TEST_CHECK_FALSE(a != b);
}

/// @brief Length 四类意图的 kind/value 契约（工厂 constexpr，边界 0/1 合法）。
AURORA_TEST_CASE(length_factories_kind_value_contract) {
    static_assert(au::Length::wrap().kind == aurora::LengthKind::WrapContent);
    static_assert(au::Length::expand().kind == aurora::LengthKind::Expand);
    static_assert(au::Length::fixed(8.0F).kind == aurora::LengthKind::Fixed);
    static_assert(au::Length::fixed(8.0F).value == 8.0F);
    static_assert(au::Length::ratio(0.0F).value == 0.0F);
    static_assert(au::Length::ratio(1.0F).kind == aurora::LengthKind::Fraction);
    AURORA_TEST_CHECK_EQ(au::Length::fixed(8.0F).value, 8.0F);
    AURORA_TEST_CHECK_EQ(au::Length::ratio(1.0F).value, 1.0F);
    AURORA_TEST_CHECK_EQ(au::Length{}.kind, aurora::LengthKind::WrapContent);
    // ⚠️ fixed(负值) / ratio(>1) 属 AURORA_ASSERT（debug-only）前置条件违例，Debug 下触发即 abort；
    //    Release（NDEBUG）下被裁切不校验，均不在单元测试中触探该路径。
}

/// @brief 编译期契约：裸标量不可隐式转为 Length（强类型尺寸意图的核心约束）。
AURORA_TEST_CASE(length_rejects_raw_scalar_implicit_conversion) {
    static_assert(!std::is_convertible_v<int, aurora::Length>);
    static_assert(!std::is_convertible_v<unsigned int, aurora::Length>);
    static_assert(!std::is_convertible_v<float, aurora::Length>);
    static_assert(!std::is_convertible_v<double, aurora::Length>);
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_types
