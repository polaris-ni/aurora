/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_paint.h
/// 测试说明: 绘制修饰节点属性与参数降级单元测试

#include <vector>

#include "aurora/modifier/modifier_paint.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_modifier_paint {

namespace {

using au::Background;
using au::BlendMode;
using au::BlendNode;
using au::BlurNode;
using au::CacheLayerNode;
using au::Clip;
using au::ClipRounded;
using au::Color;
using au::GradientBackground;
using au::ModifierNode;
using au::OpacityNode;
using au::ShadowNode;
using au::ShaderMaskKind;
using au::ShaderMaskNode;

}  // namespace

AURORA_TEST() {
    // ---- 1. Background：颜色/圆角与 PaintKind 鉴别 ----
    {
        const Background bg{Color::red(), 8.0F};
        AURORA_TEST_CHECK(bg.kind() == ModifierNode::Kind::Paint);
        AURORA_TEST_CHECK(bg.paint_kind() == ModifierNode::PaintKind::Background);
        AURORA_TEST_CHECK(bg.color().r == 255);
        AURORA_TEST_CHECK(bg.corner_radius() == 8.0F);

        const Background neg{Color::black(), -5.0F};
        AURORA_TEST_CHECK(neg.corner_radius() == 0.0F);  // 负圆角降级
    }

    // ---- 2. GradientBackground：线性带角度，径向无角度 ----
    {
        const GradientBackground linear{{Color::white(), Color::black()}, {0.0F, 1.0F}, 90.0F};
        AURORA_TEST_CHECK(linear.type() == GradientBackground::Type::Linear);
        AURORA_TEST_CHECK(linear.colors().size() == 2);
        AURORA_TEST_CHECK(linear.stops().size() == 2);
        AURORA_TEST_CHECK(linear.angle() == 90.0F);
        AURORA_TEST_CHECK(linear.paint_kind() == ModifierNode::PaintKind::GradientBackground);

        const GradientBackground radial{{Color::blue()}, {0.5F}};
        AURORA_TEST_CHECK(radial.type() == GradientBackground::Type::Radial);
        AURORA_TEST_CHECK(radial.angle() == 0.0F);
    }

    // ---- 3. ShadowNode：偏移/模糊/颜色，负模糊降级 ----
    {
        const ShadowNode sh{2.0F, 3.0F, 6.0F, Color::black()};
        AURORA_TEST_CHECK(sh.paint_kind() == ModifierNode::PaintKind::Shadow);
        AURORA_TEST_CHECK(sh.offset_x() == 2.0F);
        AURORA_TEST_CHECK(sh.offset_y() == 3.0F);
        AURORA_TEST_CHECK(sh.blur() == 6.0F);

        const ShadowNode neg{0.0F, 0.0F, -1.0F, Color::black()};
        AURORA_TEST_CHECK(neg.blur() == 0.0F);
    }

    // ---- 4. BlendNode：strength 夹取到 [0,1] ----
    {
        const BlendNode b{BlendMode::Multiply, Color::gray(), 0.4F};
        AURORA_TEST_CHECK(b.paint_kind() == ModifierNode::PaintKind::Blend);
        AURORA_TEST_CHECK(b.mode() == BlendMode::Multiply);
        AURORA_TEST_CHECK(b.strength() == 0.4F);

        const BlendNode over{BlendMode::Multiply, Color::gray(), 3.0F};
        AURORA_TEST_CHECK(over.strength() == 1.0F);
        const BlendNode under{BlendMode::Multiply, Color::gray(), -2.0F};
        AURORA_TEST_CHECK(under.strength() == 0.0F);
    }

    // ---- 5. ShaderMaskNode：strength 同样夹取 ----
    {
        const ShaderMaskNode m{ShaderMaskKind::LinearFade, 1.5F};
        AURORA_TEST_CHECK(m.paint_kind() == ModifierNode::PaintKind::ShaderMask);
        AURORA_TEST_CHECK(m.mask_kind() == ShaderMaskKind::LinearFade);
        AURORA_TEST_CHECK(m.strength() == 1.0F);
    }

    // ---- 6. CacheLayerNode：PaintKind 鉴别 ----
    {
        const CacheLayerNode cache;
        AURORA_TEST_CHECK(cache.kind() == ModifierNode::Kind::Paint);
        AURORA_TEST_CHECK(cache.paint_kind() == ModifierNode::PaintKind::CacheLayer);
    }

    // ---- 7. Clip / ClipRounded：圆角负值降级 ----
    {
        const Clip c;
        AURORA_TEST_CHECK(c.paint_kind() == ModifierNode::PaintKind::Clip);

        const ClipRounded cr{12.0F};
        AURORA_TEST_CHECK(cr.paint_kind() == ModifierNode::PaintKind::ClipRounded);
        AURORA_TEST_CHECK(cr.radius() == 12.0F);

        const ClipRounded neg{-3.0F};
        AURORA_TEST_CHECK(neg.radius() == 0.0F);
    }

    // ---- 8. OpacityNode：alpha 夹取到 [0,1] ----
    {
        const OpacityNode o{0.35F};
        AURORA_TEST_CHECK(o.alpha() == 0.35F);
        const OpacityNode hi{9.0F};
        AURORA_TEST_CHECK(hi.alpha() == 1.0F);
        const OpacityNode lo{-9.0F};
        AURORA_TEST_CHECK(lo.alpha() == 0.0F);
    }

    // ---- 9. BlurNode：半径与背景模糊标记 ----
    {
        const BlurNode b{4.5F};
        AURORA_TEST_CHECK(b.paint_kind() == ModifierNode::PaintKind::Blur);
        AURORA_TEST_CHECK(b.radius() == 4.5F);

        const BlurNode neg{-2.0F};
        AURORA_TEST_CHECK(neg.radius() == 0.0F);
    }
}

}  // namespace aurora::test_cases::utest_modifier_paint
