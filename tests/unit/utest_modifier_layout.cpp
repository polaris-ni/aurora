/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_layout.h
/// 测试说明: 覆盖 Layout 切片四节点——Padding 收缩约束并加回尺寸、PaddingEdges 非对称内边距与
/// 负值降级、FlexWeight 透明透传与权重上报、SizeModifier 固定/填充尺寸的夹取与回填

#include "aurora/modifier/modifier_layout.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier_layout {

namespace {

/// 记录收到的约束并返回固定尺寸的子测量回调。
struct FixedChild {
    float w = 0.0F;
    float h = 0.0F;
    std::optional<Constraints> seen;

    auto make_fn() -> std::function<Size(const Constraints&)> {
        return [this](const Constraints& c) -> Size {
            seen = c;
            return Size{.width = w, .height = h};
        };
    }
};

auto inner_constraints(float min_w, float min_h, float max_w, float max_h) -> Constraints {
    return Constraints{.min = Size{.width = min_w, .height = min_h}, .max = Size{.width = max_w, .height = max_h}};
}

}  // namespace

AURORA_TEST_CASE(padding_clamps_negative_to_zero) {
    // 负内边距降级为 0（Diagnostics::degraded 记录后钳制）。
    const Padding p(-5.0F);
    AURORA_TEST_CHECK_NEAR(p.padding(), 0.0F, 0.0F);
    const Padding ok(8.0F);
    AURORA_TEST_CHECK_NEAR(ok.padding(), 8.0F, 0.0F);
}

AURORA_TEST_CASE(padding_shrinks_constraints_and_adds_back) {
    // 子约束四周各收缩 pad；测量结果加回 2*pad。
    const Padding p(10.0F);
    FixedChild child{.w = 50.0F, .h = 30.0F};
    const Size s = p.layout(inner_constraints(100.0F, 80.0F, 200.0F, 160.0F), child.make_fn());

    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.height, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.width, 180.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.height, 140.0F, 0.0F);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(s.width, 70.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 50.0F, 0.0F);
}

AURORA_TEST_CASE(padding_floor_at_zero_when_constraint_smaller) {
    // 约束小于 2*pad 时子约束钳到 0（不出现负约束）。
    const Padding p(10.0F);
    FixedChild child{.w = 5.0F, .h = 5.0F};
    const Size s = p.layout(inner_constraints(0.0F, 0.0F, 10.0F, 8.0F), child.make_fn());
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.width, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.height, 0.0F, 0.0F);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(s.width, 25.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 25.0F, 0.0F);
}

AURORA_TEST_CASE(padding_edges_clamps_negative_components) {
    EdgeInsets ins{.left = -1.0F, .top = 2.0F, .right = 3.0F, .bottom = -4.0F};
    const PaddingEdges pe(ins);
    const EdgeInsets got = pe.insets();
    AURORA_TEST_CHECK_NEAR(got.left, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(got.top, 2.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(got.right, 3.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(got.bottom, 0.0F, 0.0F);
}

AURORA_TEST_CASE(padding_edges_asymmetric_layout) {
    // left+right=3、top+bottom=7：子约束水平收缩 3、垂直收缩 7，结果加回对应轴。
    const PaddingEdges pe(EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 2.0F, .bottom = 5.0F});
    FixedChild child{.w = 40.0F, .h = 30.0F};
    const Size s = pe.layout(inner_constraints(50.0F, 50.0F, 100.0F, 100.0F), child.make_fn());
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.width, 97.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.height, 93.0F, 0.0F);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(s.width, 43.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 37.0F, 0.0F);
}

AURORA_TEST_CASE(flex_weight_reports_weight_and_passthrough) {
    const FlexWeight fw(2.5F);
    AURORA_TEST_CHECK_NEAR(fw.flex_weight(), 2.5F, 0.0F);
    AURORA_TEST_CHECK_EQ(fw.kind(), ModifierNode::Kind::Layout);
    // 自身不改尺寸：约束透传、结果透传。
    FixedChild child{.w = 60.0F, .h = 20.0F};
    const Size s = fw.layout(inner_constraints(0.0F, 0.0F, 100.0F, 100.0F), child.make_fn());
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.width, 100.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.width, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 20.0F, 0.0F);
}

AURORA_TEST_CASE(size_modifier_fixed_width_and_height) {
    SizeModifier sm;
    sm.set_width(80.0F);
    sm.set_height(30.0F);
    FixedChild child{.w = 10.0F, .h = 10.0F};
    const Size s = sm.layout(inner_constraints(0.0F, 0.0F, 200.0F, 200.0F), child.make_fn());
    // 子约束被夹成 [v, v]；结果回填固定值（不受子测量影响）。
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.height, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.height, 30.0F, 0.0F);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(s.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 30.0F, 0.0F);
}

AURORA_TEST_CASE(size_modifier_fill_uses_parent_max) {
    SizeModifier sm;
    sm.set_fill_w(true);
    sm.set_fill_h(true);
    FixedChild child{.w = 10.0F, .h = 10.0F};
    const Size s = sm.layout(inner_constraints(0.0F, 0.0F, 150.0F, 90.0F), child.make_fn());
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.width, 150.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.seen.value().min.height, 90.0F, 0.0F);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(s.width, 150.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 90.0F, 0.0F);
}

AURORA_TEST_CASE(size_modifier_unconstrained_axes_passthrough) {
    // 未设置的轴（-1）沿用父约束与子测量结果。
    SizeModifier sm;
    sm.set_width(70.0F);
    FixedChild child{.w = 25.0F, .h = 35.0F};
    const Size s = sm.layout(inner_constraints(0.0F, 0.0F, 120.0F, 120.0F), child.make_fn());
    AURORA_TEST_REQUIRE(child.seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_NEAR(child.seen.value().max.height, 120.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.width, 70.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 35.0F, 0.0F);
}

AURORA_TEST_CASE(size_modifier_fill_takes_precedence_over_fixed) {
    // 同轴同时设置 fill 与固定值：fill 优先。
    SizeModifier sm;
    sm.set_width(50.0F);
    sm.set_fill_w(true);
    FixedChild child{.w = 10.0F, .h = 10.0F};
    const Size s = sm.layout(inner_constraints(0.0F, 0.0F, 90.0F, 90.0F), child.make_fn());
    AURORA_TEST_CHECK_NEAR(s.width, 90.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 10.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_modifier_layout
