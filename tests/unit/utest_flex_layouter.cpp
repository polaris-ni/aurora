/// 测试类型: unit
/// 目标单元: include/aurora/layout/flex_layouter.h
/// 测试说明: 覆盖 Flex 布局算法的 Flutter 语义——Row/Column 主轴排布、RowReverse 镜像、
/// flex 权重瓜分剩余空间、gap 插入与分配扣除、MainAxisSize::Max 下的主轴对齐
/// （Center/End/SpaceBetween/SpaceAround/SpaceEvenly）、交叉轴对齐（Start/Center/End/Stretch/Baseline
/// 含无基线子项的合成基线、全无基线退化为 End、Column 回退 Start、RTL 正交性、父约束夹取）

#include <optional>
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
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
}

/// 带基线通道的测量上下文：`baseline` 为「布局盒顶 → 首行基线」距离；< 0 表示无基线（nullopt）。
struct BaselineCtx : aurora::LayoutCtxBase {
    float w = 0.0F;
    float h = 0.0F;
    float baseline = -1.0F;
};

auto baseline_measure(void *ctx, const Constraints &c) -> Size {
    const auto *self = static_cast<BaselineCtx *>(ctx);
    return c.constrain(Size{.width = self->w, .height = self->h});
}

auto baseline_of(void *ctx, Size /*measured*/) -> std::optional<float> {
    const auto *self = static_cast<BaselineCtx *>(ctx);
    if (self->baseline < 0.0F) {
        return std::nullopt;
    }
    return self->baseline;
}

/// 带基线通道的子项（baseline < 0 = 无基线，走 CSS 式合成基线 = 交叉轴底边）。
auto baseline_item(float w, float h, float baseline, BaselineCtx &ctx) -> FlexItem {
    ctx.w = w;
    ctx.h = h;
    ctx.baseline = baseline;
    return FlexItem::make(0.0F, &ctx, &baseline_measure, &baseline_of, &ctx);
}

}  // namespace

AURORA_TEST_CASE(row_start_places_fixed_children_left_to_right) {
    FixedCtx a;
    FixedCtx b;
    const auto layout = FlexLayouter::layout(Flex{}, parent_constraints(),
                                             {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 10.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(column_uses_vertical_main_axis) {
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.direction = FlexDirection::Column;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 20.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(row_reverse_mirrors_positions_within_content_width) {
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.direction = FlexDirection::RowReverse;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // MainAxisSize::Min：容器主轴=内容 60，镜像发生在 60 宽内 → 首项贴右端。
    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(flex_weights_split_free_space_proportionally) {
    FixedCtx fix;
    FixedCtx w1;
    FixedCtx w3;
    // 剩余 300-30=270 按权重 1:3 瓜分 → 67.5 / 202.5；容器主轴撑满分配后的 300。
    // flex 项期望宽给大值，被 max=分配额 的约束夹到 67.5/202.5。
    const auto layout = FlexLayouter::layout(
        Flex{}, parent_constraints(),
        {flex_item(1.0F, 1e9F, 10.0F, w1), fixed_item2(30.0F, 10.0F, fix), flex_item(3.0F, 1e9F, 10.0F, w3)});
    AURORA_TEST_CHECK_NEAR(layout.size.width, 300.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 3U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].size.width, 67.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].size.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 67.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[2].size.width, 202.5F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(layout.children[2].origin.x, 97.5F, 1e-3F);
}

AURORA_TEST_CASE(gap_inserts_between_children_and_counts_into_size) {
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.gap = 10.0F;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // 相邻间距 10 计入容器主轴：60+10=70；首个子项前不加 gap。
    AURORA_TEST_CHECK_NEAR(layout.size.width, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(flex_split_deducts_gap_before_allocating) {
    FixedCtx fix;
    FixedCtx w1;
    Flex cfg;
    cfg.gap = 10.0F;
    // 剩余 300-30-10=260 全归 flex 项（期望宽大值被夹到分配额）；固定项在前，flex 项 x = 30+10=40。
    const auto layout = FlexLayouter::layout(cfg, parent_constraints(),
                                             {fixed_item2(30.0F, 10.0F, fix), flex_item(1.0F, 1e9F, 10.0F, w1)});
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
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.main_axis_size = MainAxisSize::Max;
    cfg.main_axis = MainAxisAlignment::SpaceBetween;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // free=240 全给间隔：x=0 与 x=30+240=270。
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 270.0F, 1e-4F);
}

AURORA_TEST_CASE(space_around_and_evenly_distribute_free_space) {
    FixedCtx a;
    FixedCtx b;
    Flex cfg_around;
    cfg_around.main_axis_size = MainAxisSize::Max;
    cfg_around.main_axis = MainAxisAlignment::SpaceAround;
    const auto around = FlexLayouter::layout(cfg_around, parent_constraints(),
                                             {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // SpaceAround（free=240，n=2）：leading=free/(2n)=60、between=free/n=120 → x=60 / 210。
    AURORA_TEST_CHECK_NEAR(around.children[0].origin.x, 60.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(around.children[1].origin.x, 210.0F, 1e-3F);

    Flex cfg_evenly;
    cfg_evenly.main_axis_size = MainAxisSize::Max;
    cfg_evenly.main_axis = MainAxisAlignment::SpaceEvenly;
    const auto evenly = FlexLayouter::layout(cfg_evenly, parent_constraints(),
                                             {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});

    // SpaceEvenly：n+1=3 段均分 240/3=80 → x=80 / 190。
    AURORA_TEST_CHECK_NEAR(evenly.children[0].origin.x, 80.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(evenly.children[1].origin.x, 190.0F, 1e-3F);
}

AURORA_TEST_CASE(cross_axis_center_end_stretch_align_within_container_cross) {
    FixedCtx small;
    FixedCtx tall;
    Flex center_cfg;
    center_cfg.cross_axis = CrossAxisAlignment::Center;
    const auto centered = FlexLayouter::layout(center_cfg, parent_constraints(),
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
    const auto stretched = FlexLayouter::layout(stretch_cfg, parent_constraints(),
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
    const auto layout = FlexLayouter::layout(
        Flex{},
        Constraints{.min = Size{.width = 100.0F, .height = 50.0F}, .max = Size{.width = 300.0F, .height = 100.0F}},
        {fixed_item2(30.0F, 10.0F, a)});

    AURORA_TEST_CHECK_NEAR(layout.size.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 50.0F, 1e-4F);
}

AURORA_TEST_CASE(row_rtl_mirrors_child_order_and_start_end) {
    // 布局镜像：Row(rtl) 的 Start 排布镜像后首项贴右缘（视觉顺序翻转）。
    // LTR：a@0, b@30（容器 60）；RTL：a@30, b@0。
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.rtl = true;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(30.0F, 10.0F, b)});
    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
    // 子项尺寸不受镜像影响。
    AURORA_TEST_CHECK_NEAR(layout.children[0].size.width, 30.0F, 1e-4F);

    // Max 容器 + End 对齐：LTR 末项贴右缘（x=270），RTL 镜像后贴左缘（x=0）——End 语义对调。
    FixedCtx c;
    Flex cfg2;
    cfg2.main_axis_size = MainAxisSize::Max;
    cfg2.main_axis = MainAxisAlignment::End;
    cfg2.rtl = true;
    const auto layout2 = FlexLayouter::layout(cfg2, parent_constraints(), {fixed_item2(30.0F, 10.0F, c)});
    AURORA_TEST_CHECK_NEAR(layout2.size.width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout2.children[0].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(column_rtl_mirrors_cross_axis_start_end) {
    // 布局镜像：Column(rtl) 交叉轴（水平）Start 对齐镜像为视觉上的右缘对齐。
    // 窄项(30) + 宽项(100)：容器交叉宽 100；LTR Start 窄项 x=0，RTL 镜像后 x=70。
    FixedCtx a;
    FixedCtx b;
    Flex cfg;
    cfg.direction = FlexDirection::Column;
    cfg.rtl = true;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(), {fixed_item2(30.0F, 10.0F, a), fixed_item2(100.0F, 10.0F, b)});
    AURORA_TEST_CHECK_NEAR(layout.size.width, 100.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
    // 纵向排布顺序不变（主轴不受镜像影响）。
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 10.0F, 1e-4F);
}

AURORA_TEST_CASE(baseline_aligns_mixed_sizes_on_common_line) {
    // 大字号（h=30, b=24）与小字号（h=20, b=15）：容器交叉轴 = max_above(24) + max_below(max(6,5)=6) = 30；
    // 小字号整体下移 24-15=9，两者首行基线同处 y=24。
    BaselineCtx small;
    BaselineCtx large;
    Flex cfg;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(),
                             {baseline_item(30.0F, 20.0F, 15.0F, small), baseline_item(30.0F, 30.0F, 24.0F, large)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 9.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 0.0F, 1e-4F);
    // 基线共线：各自 y + 自身 baseline 相等（基线对齐的定义式断言）。
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y + 15.0F, layout.children[1].origin.y + 24.0F, 1e-4F);
    // 主轴排布不受交叉轴对齐影响。
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 30.0F, 1e-4F);
    // 子项尺寸不被拉伸（Baseline 不是 Stretch）。
    AURORA_TEST_CHECK_NEAR(layout.children[0].size.height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(baseline_synthesizes_bottom_edge_for_items_without_baseline) {
    // 无基线子项（图标类）按 CSS 式合成基线 = 自身交叉轴底边：h=40 的项 b=40；
    // max_above = max(15, 40) = 40、max_below = max(5, 0) = 5 ⇒ 容器交叉轴 45。
    BaselineCtx text;
    BaselineCtx icon;
    Flex cfg;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(),
                             {baseline_item(30.0F, 20.0F, 15.0F, text), baseline_item(20.0F, 40.0F, -1.0F, icon)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 45.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 25.0F, 1e-4F);  // 40 - 15
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 0.0F, 1e-4F);  // 40 - 40：合成基线贴容器底
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y + 40.0F, 40.0F, 1e-4F);
}

AURORA_TEST_CASE(all_items_without_baseline_degrade_to_end_alignment) {
    // 全部子项无基线 ⇒ max_above = max(cross_size)、max_below = 0 ⇒ cross_pos = 容器交叉轴 - 自身交叉尺寸，
    // 与 End 语义逐位一致（宽容降级，不报错、不崩溃）。
    BaselineCtx a;
    BaselineCtx b;
    Flex baseline_cfg;
    baseline_cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto degraded =
        FlexLayouter::layout(baseline_cfg, parent_constraints(),
                             {baseline_item(10.0F, 5.0F, -1.0F, a), baseline_item(30.0F, 10.0F, -1.0F, b)});

    FixedCtx ca;
    FixedCtx cb;
    Flex end_cfg;
    end_cfg.cross_axis = CrossAxisAlignment::End;
    const auto ended = FlexLayouter::layout(end_cfg, parent_constraints(),
                                            {fixed_item2(10.0F, 5.0F, ca), fixed_item2(30.0F, 10.0F, cb)});

    AURORA_TEST_REQUIRE_EQ(degraded.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(degraded.size.height, ended.size.height, 1e-4F);
    AURORA_TEST_CHECK_NEAR(degraded.children[0].origin.y, ended.children[0].origin.y, 1e-4F);
    AURORA_TEST_CHECK_NEAR(degraded.children[1].origin.y, ended.children[1].origin.y, 1e-4F);
}

AURORA_TEST_CASE(baseline_on_column_falls_back_to_start) {
    // Column 的交叉轴是水平的：基线无意义 ⇒ 按 Start（x=0），且容器交叉轴仍取最大宽。
    BaselineCtx a;
    BaselineCtx b;
    Flex cfg;
    cfg.direction = FlexDirection::Column;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(), {baseline_item(30.0F, 10.0F, 8.0F, a), baseline_item(60.0F, 10.0F, 8.0F, b)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 10.0F, 1e-4F);
}

AURORA_TEST_CASE(baseline_ignores_rtl_mirror_on_vertical_axis) {
    // 基线与 RTL 正交：镜像只改主轴 x，交叉轴基线定位逐位不变。
    BaselineCtx small;
    BaselineCtx large;
    Flex cfg;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    cfg.rtl = true;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(),
                             {baseline_item(30.0F, 20.0F, 15.0F, small), baseline_item(30.0F, 30.0F, 24.0F, large)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 9.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.x, 30.0F, 1e-4F);  // 已镜像
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(baseline_clamps_out_of_range_baseline_into_item_box) {
    // 越界基线（远大于盒高）夹取到 [0, cross_size]：999 → 60 ⇒ 等价于「底边合成基线」，
    // cross_pos = 60 - 60 = 0（若不夹取将得到负位移，把子项顶出容器上沿）。
    BaselineCtx odd;
    Flex cfg;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto layout =
        FlexLayouter::layout(cfg, parent_constraints(300.0F, 100.0F), {baseline_item(30.0F, 60.0F, 999.0F, odd)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 1U);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(baseline_container_cross_clamps_into_parent_constraint) {
    // 基线在盒底（above=70/below=0）与基线在盒顶（above=0/below=70）混排时，
    // max_above + max_below = 140，可超过父交叉轴约束 70 ⇒ 容器夹到父 max；
    // 子项位移仍非负（此项越界属父约束给定的退化空间，不做额外裁剪）。
    BaselineCtx bottom_heavy;
    BaselineCtx top_heavy;
    Flex cfg;
    cfg.cross_axis = CrossAxisAlignment::Baseline;
    const auto layout = FlexLayouter::layout(
        cfg, parent_constraints(300.0F, 70.0F),
        {baseline_item(30.0F, 70.0F, 70.0F, bottom_heavy), baseline_item(30.0F, 70.0F, 0.0F, top_heavy)});

    AURORA_TEST_REQUIRE_EQ(layout.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(layout.size.height, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(layout.children[0].origin.y, 0.0F, 1e-4F);  // 70 - 70
    AURORA_TEST_CHECK_NEAR(layout.children[1].origin.y, 70.0F, 1e-4F);  // 70 - 0
    AURORA_TEST_CHECK_GE(layout.children[1].origin.y, 0.0F);
}

}  // namespace aurora::test_cases::utest_flex_layouter
