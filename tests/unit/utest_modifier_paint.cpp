/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_paint.h
/// 测试说明: 覆盖 Paint 切片全部节点——kind/paint_kind 分发、参数回读、负值钳制
/// （radius/blur/border）、strength/alpha 的 [0,1] 夹取、渐变类型与色标回读、layout 透传

#include "aurora/modifier/modifier.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier_paint {

namespace {

auto passthrough(const Constraints& c) -> Size { return c.constrain(Size{.width = 42.0F, .height = 24.0F}); }

auto constraints(float max_w = 100.0F, float max_h = 100.0F) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
}

}  // namespace

AURORA_TEST_CASE(background_keeps_color_radius_and_clamps_negative_radius) {
    const Background b(Color(10, 20, 30, 255), 8.0F);
    AURORA_TEST_CHECK_EQ(b.kind(), ModifierNode::Kind::Paint);
    AURORA_TEST_CHECK_EQ(b.paint_kind(), ModifierNode::PaintKind::Background);
    AURORA_TEST_CHECK_EQ(b.color().r, 10);
    AURORA_TEST_CHECK_EQ(b.corner_radius(), 8.0F);
    // 负 radius 钳为 0。
    const Background neg(Color(0, 0, 0, 255), -3.0F);
    AURORA_TEST_CHECK_NEAR(neg.corner_radius(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(gradient_linear_stores_colors_stops_angle) {
    const GradientBackground g(std::vector{Color(255, 0, 0, 255), Color(0, 0, 255, 255)}, std::vector{0.0F, 1.0F},
                               45.0F);
    AURORA_TEST_CHECK_EQ(g.paint_kind(), ModifierNode::PaintKind::GradientBackground);
    AURORA_TEST_CHECK_EQ(g.type(), GradientBackground::Type::Linear);
    AURORA_TEST_REQUIRE_EQ(g.colors().size(), 2U);
    AURORA_TEST_CHECK_EQ(g.colors()[0].r, 255);
    AURORA_TEST_CHECK_EQ(g.colors()[1].b, 255);
    AURORA_TEST_REQUIRE_EQ(g.stops().size(), 2U);
    AURORA_TEST_CHECK_NEAR(g.angle(), 45.0F, 0.0F);
}

AURORA_TEST_CASE(gradient_radial_default_angle_zero) {
    const GradientBackground g(std::vector{Color(0, 0, 0, 255), Color(255, 255, 255, 255)}, std::vector{0.0F, 1.0F});
    AURORA_TEST_CHECK_EQ(g.type(), GradientBackground::Type::Radial);
    AURORA_TEST_CHECK_NEAR(g.angle(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(shadow_keeps_offset_color_and_clamps_negative_blur) {
    const ShadowNode s(2.0F, 3.0F, 6.0F, Color(0, 0, 0, 64));
    AURORA_TEST_CHECK_EQ(s.paint_kind(), ModifierNode::PaintKind::Shadow);
    AURORA_TEST_CHECK_NEAR(s.offset_x(), 2.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.offset_y(), 3.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.blur(), 6.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(s.color().a, 64);
    const ShadowNode neg(0.0F, 0.0F, -1.0F, Color(0, 0, 0, 0));
    AURORA_TEST_CHECK_NEAR(neg.blur(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(blend_clamps_strength_into_unit_range) {
    const BlendNode over(BlendMode::Overlay, Color(255, 0, 0, 255), 0.7F);
    AURORA_TEST_CHECK_EQ(over.paint_kind(), ModifierNode::PaintKind::Blend);
    AURORA_TEST_CHECK_EQ(over.mode(), BlendMode::Overlay);
    AURORA_TEST_CHECK_EQ(over.tint().g, 0);
    AURORA_TEST_CHECK_NEAR(over.strength(), 0.7F, 0.0F);
    const BlendNode hi(BlendMode::Normal, Color(0, 0, 0, 255), 2.0F);
    AURORA_TEST_CHECK_NEAR(hi.strength(), 1.0F, 0.0F);
    const BlendNode lo(BlendMode::Multiply, Color(0, 0, 0, 255), -0.5F);
    AURORA_TEST_CHECK_NEAR(lo.strength(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(shader_mask_clamps_strength_and_keeps_kind) {
    const ShaderMaskNode fade(ShaderMaskKind::LinearFade, 0.8F);
    AURORA_TEST_CHECK_EQ(fade.paint_kind(), ModifierNode::PaintKind::ShaderMask);
    AURORA_TEST_CHECK_EQ(fade.mask_kind(), ShaderMaskKind::LinearFade);
    AURORA_TEST_CHECK_NEAR(fade.strength(), 0.8F, 0.0F);
    const ShaderMaskNode hi(ShaderMaskKind::RadialFade, 5.0F);
    AURORA_TEST_CHECK_NEAR(hi.strength(), 1.0F, 0.0F);
}

AURORA_TEST_CASE(cache_layer_reports_paint_kind) {
    const CacheLayerNode c;
    AURORA_TEST_CHECK_EQ(c.kind(), ModifierNode::Kind::Paint);
    AURORA_TEST_CHECK_EQ(c.paint_kind(), ModifierNode::PaintKind::CacheLayer);
}

AURORA_TEST_CASE(border_keeps_width_color_and_clamps_negative) {
    const Border b(2.0F, Color(255, 255, 0, 255));
    AURORA_TEST_CHECK_EQ(b.paint_kind(), ModifierNode::PaintKind::Border);
    AURORA_TEST_CHECK_NEAR(b.border_width(), 2.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(b.border_color().b, 0);
    const Border neg(-1.0F, Color(0, 0, 0, 255));
    AURORA_TEST_CHECK_NEAR(neg.border_width(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(clip_and_clip_rounded_paint_kinds) {
    const Clip c;
    AURORA_TEST_CHECK_EQ(c.paint_kind(), ModifierNode::PaintKind::Clip);
    const ClipRounded r(12.0F);
    AURORA_TEST_CHECK_EQ(r.paint_kind(), ModifierNode::PaintKind::ClipRounded);
    AURORA_TEST_CHECK_NEAR(r.radius(), 12.0F, 0.0F);
    const ClipRounded neg(-2.0F);
    AURORA_TEST_CHECK_NEAR(neg.radius(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(opacity_clamps_alpha_into_unit_range) {
    // OpacityNode 不覆盖 paint_kind（走独立透明度通道，非 PaintKind 分发）。
    const OpacityNode half(0.5F);
    AURORA_TEST_CHECK_EQ(half.kind(), ModifierNode::Kind::Paint);
    AURORA_TEST_CHECK_EQ(half.paint_kind(), ModifierNode::PaintKind::None);
    AURORA_TEST_CHECK_NEAR(half.alpha(), 0.5F, 0.0F);
    const OpacityNode hi(1.5F);
    AURORA_TEST_CHECK_NEAR(hi.alpha(), 1.0F, 0.0F);
    const OpacityNode lo(-0.2F);
    AURORA_TEST_CHECK_NEAR(lo.alpha(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(blur_keeps_radius_and_backdrop_flag) {
    const BlurNode content_blur(4.0F);
    AURORA_TEST_CHECK_EQ(content_blur.paint_kind(), ModifierNode::PaintKind::Blur);
    AURORA_TEST_CHECK_NEAR(content_blur.radius(), 4.0F, 0.0F);
    AURORA_TEST_CHECK_FALSE(content_blur.is_backdrop());
    const BlurNode backdrop(8.0F, true);
    AURORA_TEST_CHECK_TRUE(backdrop.is_backdrop());
    const BlurNode neg(-3.0F);
    AURORA_TEST_CHECK_NEAR(neg.radius(), 0.0F, 0.0F);
}

AURORA_TEST_CASE(paint_nodes_do_not_change_layout) {
    // Paint 切片不参与测量：一律透传子测量结果。
    AURORA_TEST_CHECK_NEAR(passthrough(constraints()).width, 42.0F, 0.0F);
    const Background b(Color(0, 0, 0, 255));
    const Size s1 = b.layout(constraints(), passthrough);
    AURORA_TEST_CHECK_NEAR(s1.width, 42.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s1.height, 24.0F, 0.0F);
    const Border bd(1.0F, Color(0, 0, 0, 255));
    const Size s2 = bd.layout(constraints(), passthrough);
    AURORA_TEST_CHECK_NEAR(s2.width, 42.0F, 0.0F);
    const ClipRounded cr(4.0F);
    const Size s3 = cr.layout(constraints(), passthrough);
    AURORA_TEST_CHECK_NEAR(s3.height, 24.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_modifier_paint
