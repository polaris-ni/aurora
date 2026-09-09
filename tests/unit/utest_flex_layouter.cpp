/// 测试类型: unit
/// 目标单元: include/aurora/layout/flex_layouter.h
/// 测试说明: 覆盖 Flex 布局算法的 Flutter 语义——Row/Column 主轴排布、RowReverse 镜像、
/// flex 权重瓜分剩余空间、gap 插入与分配扣除、MainAxisSize::Max 下的主轴对齐
/// （Center/End/SpaceBetween/SpaceAround/SpaceEvenly）、交叉轴对齐（Start/Center/End/Stretch）

#include <vector>

#include "aurora/layout/flex_layouter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_flex_layouter {

namespace {

/// 测量上下文：返回期望尺寸并夹入约束（模拟真实 widget 的 constrain 行为）。
struct FixedCtx : aurora::LayoutCtxBase {
    float w = 0.0F;
    float h = 0.0F;
};

auto fixed_measure(void *ctx, const Constraints &c) -> Size {
    const auto *self = static_cast<FixedCtx *>(ctx);
    return c.constrain(Size{.width = self->w, .height = self->h});
}

/// 独立上下文的固定项（同用例多个子项时每项一个 ctx）。
auto fixed_item2(float w, float h, FixedCtx &ctx) -> FlexItem {
    ctx.w = w;
    ctx.h = h;
    return FlexItem::make(0.0F, &ctx, &fixed_measure);
}

auto flex_item(float weight, float w, float h, FixedCtx &ctx) -> FlexItem {
    ctx.w = w;
    ctx.h = h;
    return FlexItem::make(weight, &ctx, &fixed_measure);
}

auto parent_constraints(float max_w = 300.0F, float max_h = 100.0F) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                       .max = Size{.width = max_w, .height = max_h}};
}

}  // namespace

AURORA_TEST_CASE(row_start_places_fixed_children_left_to_right) {
    FixedCtx a, b;
    const auto layout = FlexLayouter::layout(
        Flex{}, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 10.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(column_uses_vertical_main_axis) {
    FixedCtx a, b;
    Flex cfg;
    cfg.direction = FlexDirection::Column;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 20.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(row_reverse_mirrors_positions_within_content_width) {
    FixedCtx a, b;
    Flex cfg;
    cfg.direction = FlexDirection::RowReverse;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // MainAxisSize::Min：容器主轴=内容 60，镜像发生在 60 宽内 → 首项贴右端。
    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(flex_weights_split_free_space_proportionally) {
    FixedCtx fix, w1, w3;
    // 剩余 300-30=270 按权重 1:3 瓜分 → 67.5 / 202.5；容器主轴撑满分配后的 300。
    // flex 项期望宽给大值，被 max=分配额 的约束夹到 67.5/202.5。
    const auto layout =
        FlexLayouter::layout(Flex{}, parent_constraints(),
                             {flex_item(1.0F, 1e9F, 10.0F, w1), fixed_item2(30.0F, 10.0F, fix),
                              flex_item(3.0F, 1e9F, 10.0F, w3)});
    AURORA_TEST_CHECK_NEAR(layout.size.width, 300.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 3U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].size.width, 67.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].size.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 67.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[2].size.width, 202.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[2].origin.x, 97.5F, 1e-3F);
}

AURORA_TEST_CASE(gap_inserts_between_children_and_counts_into_size) {
    FixedCtx a, b;
    Flex cfg;
    cfg.gap = 10.0F;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // 相邻间距 10 计入容器主轴：60+10=70；首个子项前不加 gap。
    AURORA_TEST_CHECK_NEAR(layout.size.width, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(flex_split_deducts_gap_before_allocating) {
    FixedCtx fix, w1;
    Flex cfg;
    cfg.gap = 10.0F;
    // 剩余 300-30-10=260 全归 flex 项（期望宽大值被夹到分配额）；固定项在前，flex 项 x = 30+10=40。
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, fix), flex_item(1.0F, 1e9F, 10.0F, w1)});
    AURORA_TEST_CHECK_NEAR(layout.children[0].size.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].size.width, 260.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(main_axis_max_enables_alignment_free_space) {
    FixedCtx a;
    Flex cfg;
    cfg.main_axis_size = MainAxisSize::Max;
    cfg.main_axis = MainAxisAlignment::Center;
    const auto layout = FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a)});

    // 容器撑满 300，Center：x = (300-30)/2 = 135。
    AURORA_TEST_CHECK_NEAR(layout.size.width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 135.0F, 1e-4F);
}

AURORA_TEST_CASE(space_between_pins_first_and_last) {
    FixedCtx a, b;
    Flex cfg;
    cfg.main_axis_size = MainAxisSize::Max;
    cfg.main_axis = MainAxisAlignment::SpaceBetween;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // free=240 全给间隔：x=0 与 x=30+240=270。
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 270.0F, 1e-4F);
}

AURORA_TEST_CASE(space_around_and_evenly_distribute_free_space) {
    FixedCtx a, b;
    Flex cfg_around;
    cfg_around.main_axis_size = MainAxisSize::Max;
    cfg_around.main_axis = MainAxisAlignment::SpaceAround;
    const auto around = FlexLayouter::layout(
        cfg_around, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // SpaceAround（free=240，n=2）：leading=free/(2n)=60、between=free/n=120 → x=60 / 210。
    AURORA_TEST_CHECK_NEAR(around.children[0].origin.x, 60.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(around.children[1].origin.x, 210.0F, 1e-3F);

    Flex cfg_evenly;
    cfg_evenly.main_axis_size = MainAxisSize::Max;
    cfg_evenly.main_axis = MainAxisAlignment::SpaceEvenly;
    const auto evenly = FlexLayouter::layout(
        cfg_evenly, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // SpaceEvenly：n+1=3 段均分 240/3=80 → x=80 / 190。
    AURORA_TEST_CHECK_NEAR(evenly.children[0].origin.x, 80.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(evenly.children[1].origin.x, 190.0F, 1e-3F);
}

AURORA_TEST_CASE(cross_axis_center_end_stretch_align_within_container_cross) {
    FixedCtx small, tall;
    Flex center_cfg;
    center_cfg.cross_axis = CrossAxisAlignment::Center;
    const auto centered = FlexLayouter::layout(
        center_cfg, parent_constraints(),
        {fixed_item2(10.0F, 5.0F, small), fixed_item2(30.0F, 10.0F, tall)});

    // 容器交叉轴 = max(5,10)=10；Center：y=(10-5)/2=2.5 与 0。
    AURORA_TEST_CHECK_NEAR(centered.size.height, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(centered.children[0].origin.y, 2.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(centered.children[1].origin.y, 0.0F, 1e-4F);

    Flex end_cfg;
    end_cfg.cross_axis = CrossAxisAlignment::End;
    const auto ended = FlexLayouter::layout(end_cfg, parent_constraints(),
                                            {fixed_item2(10.0F, 5.0F, small), fixed_item2(30.0F, 10.0F, tall)});
    AURORA_TEST_CHECK_NEAR(ended.children[0].origin.y, 5.0F, 1e-4F);

    Flex stretch_cfg;
    stretch_cfg.cross_axis = CrossAxisAlignment::Stretch;
    const auto stretched =
        FlexLayouter::layout(stretch_cfg, parent_constraints(),
                             {fixed_item2(10.0F, 5.0F, small), fixed_item2(30.0F, 10.0F, tall)});
    // Stretch：短项高度拉伸到容器交叉轴 10，宽度不变。
    AURORA_TEST_CHECK_NEAR(stretched.children[0].size.height, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(stretched.children[0].size.width, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(stretched.children[0].origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(empty_items_yield_zero_size_and_no_children) {
    const auto layout = FlexLayouter::layout(Flex{}, parent_constraints(), {});

    AURORA_TEST_CHECK_TRUE(layout.children.empty());
    AURORA_TEST_CHECK_NEAR(layout.size.width, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 0.0F, 0.0F);
}

AURORA_TEST_CASE(container_size_clamps_into_parent_min) {
    FixedCtx a;
    // 内容 30x10，但父 min 100x50：Min 语义下容器主轴/交叉轴都夹入父 min。
    const auto layout =
        FlexLayouter::layout(Flex{}, Constraints{.min = Size{.width = 100.0F, .height = 50.0F},
                                                 .max = Size{.width = 300.0F, .height = 100.0F}},
                             {fixed_item2(30.0F, 10.0F, a)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 50.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_flex_layouter
