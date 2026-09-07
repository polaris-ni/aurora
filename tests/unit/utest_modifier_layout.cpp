/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_layout.h
/// 测试说明: 布局修饰节点（Padding / PaddingEdges / FlexWeight / SizeModifier）单元测试

#include <functional>

#include "aurora/modifier/modifier_layout.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_modifier_layout {

namespace {

using au::Constraints;
using au::EdgeInsets;
using au::FlexWeight;
using au::ModifierNode;
using au::Padding;
using au::PaddingEdges;
using au::Size;
using au::SizeModifier;

/// 子节点按紧约束（min）测量，便于观察父节点对约束的改写。
auto measure_tight() -> std::function<Size(const Constraints &)> {
    return [](const Constraints &inner) -> Size { return inner.min; };
}

auto measure_fixed(const Size &s) -> std::function<Size(const Constraints &)> {
    return [s](const Constraints & /*inner*/) -> Size { return s; };
}

}  // namespace

AURORA_TEST() {
    // ---- 1. Padding 收缩约束并加回内边距 ----
    {
        const Padding p{8.0F};
        AURORA_TEST_CHECK(p.kind() == ModifierNode::Kind::Layout);
        AURORA_TEST_CHECK(p.padding() == 8.0F);

        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 100.0F, .height = 200.0F}};
        // 子节点吃掉收缩后的可用空间（84 x 184），父节点再加回 16
        const Size s = p.layout(c, [](const Constraints &inner) -> Size { return inner.max; });
        AURORA_TEST_CHECK(s.width == 100.0F);
        AURORA_TEST_CHECK(s.height == 200.0F);
    }

    // ---- 2. Padding 负值降级为 0 ----
    {
        const Padding p{-4.0F};
        AURORA_TEST_CHECK(p.padding() == 0.0F);
    }

    // ---- 3. PaddingEdges 按轴独立收缩 ----
    {
        const PaddingEdges p{EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F}};
        AURORA_TEST_CHECK(p.insets().horizontal() == 4.0F);
        AURORA_TEST_CHECK(p.insets().vertical() == 6.0F);

        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 50.0F, .height = 60.0F}};
        float seen_w = 0.0F;
        float seen_h = 0.0F;
        const Size s = p.layout(c, [&](const Constraints &inner) -> Size {
            seen_w = inner.max.width;
            seen_h = inner.max.height;
            return inner.max;
        });
        AURORA_TEST_CHECK(seen_w == 46.0F);  // 50 - (1+3)
        AURORA_TEST_CHECK(seen_h == 54.0F);  // 60 - (2+4)
        AURORA_TEST_CHECK(s.width == 50.0F);
        AURORA_TEST_CHECK(s.height == 60.0F);
    }

    // ---- 4. PaddingEdges 任一负值降级为 0 ----
    {
        const PaddingEdges p{EdgeInsets{.left = 5.0F, .top = -1.0F, .right = 2.0F, .bottom = 0.0F}};
        AURORA_TEST_CHECK(p.insets().top == 0.0F);
        AURORA_TEST_CHECK(p.insets().left == 5.0F);
        AURORA_TEST_CHECK(p.insets().vertical() == 0.0F);
    }

    // ---- 5. FlexWeight 透传尺寸但暴露权重 ----
    {
        const FlexWeight f{2.0F};
        AURORA_TEST_CHECK(f.flex_weight() == 2.0F);

        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 300.0F, .height = 100.0F}};
        const Size s = f.layout(c, measure_fixed(Size{.width = 42.0F, .height = 24.0F}));
        AURORA_TEST_CHECK(s.width == 42.0F);  // 不改变子节点尺寸
        AURORA_TEST_CHECK(s.height == 24.0F);
    }

    // ---- 6. SizeModifier 固定宽高 ----
    {
        SizeModifier m;
        m.set_width(50.0F);
        m.set_height(30.0F);
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 100.0F, .height = 100.0F}};
        const Size s = m.layout(c, measure_tight());
        AURORA_TEST_CHECK(s.width == 50.0F);
        AURORA_TEST_CHECK(s.height == 30.0F);
    }

    // ---- 7. SizeModifier fill 取约束上限，优先于固定尺寸 ----
    {
        SizeModifier m;
        m.set_width(50.0F);  // 应被 fill 覆盖
        m.set_fill_w(true);
        m.set_fill_h(true);
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 320.0F, .height = 240.0F}};
        const Size s = m.layout(c, measure_tight());
        AURORA_TEST_CHECK(s.width == 320.0F);
        AURORA_TEST_CHECK(s.height == 240.0F);
    }

    // ---- 8. SizeModifier 未设置时沿用子节点尺寸 ----
    {
        const SizeModifier m;
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = 100.0F, .height = 100.0F}};
        const Size s = m.layout(c, measure_fixed(Size{.width = 7.0F, .height = 9.0F}));
        AURORA_TEST_CHECK(s.width == 7.0F);
        AURORA_TEST_CHECK(s.height == 9.0F);
    }
}

}  // namespace aurora::test_cases::utest_modifier_layout
