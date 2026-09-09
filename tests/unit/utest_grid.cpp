/// 测试类型: unit
/// 目标单元: include/aurora/widget/grid.h
/// 测试说明: 覆盖 Grid 网格布局——行优先落位与列宽/行高聚合、间距计入总尺寸、
/// 有界宽度均分单元格、set_columns/set_gap 钳制、属性序列化往返、自描述

#include <memory>

#include "aurora/widget/grid.h"
#include "aurora/widget/text.h"
#include "aurora/layout/layout_engine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_grid {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto box_expand_w(float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::expand());
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(grid_places_row_major_with_per_column_width) {
    // 2 列、gap=10：A(50x20) B(30x10) / C(20x30)。
    // 列宽 [50,30]、行高 [20,30]；总尺寸 90x60。
    Grid grid{GridProps{.children = {box(50.0F, 20.0F), box(30.0F, 10.0F), box(20.0F, 30.0F)}, .columns = 2, .gap = 10.0F}};
    AURORA_TEST_CHECK_EQ(std::string{grid.type_name()}, "Grid");

    LayoutEngine::layout(grid, bounded(500.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 90.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 60.0F, 1e-4F);

    const auto &kids = grid.child_nodes();
    AURORA_TEST_REQUIRE_EQ(kids.size(), 3U);
    // idx0 (r0,c0)：(0,0)；idx1 (r0,c1)：x=50+10=60；idx2 (r1,c0)：y=20+10=30。
    AURORA_TEST_CHECK_NEAR(kids[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[0].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[1].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[1].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[2].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[2].bounds().origin.y, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_bounded_width_equalizes_cell_measurement) {
    // 有界宽 200、2 列、gap=10 → 单元格测量宽 (200-10)/2=95：
    // Expand 子项被撑到 95，总宽 95+95+10=200。
    Grid grid{GridProps{.children = {box_expand_w(20.0F), box_expand_w(30.0F)}, .columns = 2, .gap = 10.0F}};
    LayoutEngine::layout(grid, bounded(200.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[0].bounds().size.width, 95.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[1].bounds().size.width, 95.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_handles_uneven_last_row) {
    // 3 项 2 列：第二行只有 1 项，不崩溃且行高取该行实际子项。
    Grid grid{GridProps{.children = {box(50.0F, 20.0F), box(50.0F, 20.0F), box(50.0F, 40.0F)},
                        .columns = 2,
                        .gap = 4.0F}};
    LayoutEngine::layout(grid, bounded(500.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 104.0F, 1e-4F);  // 50+50+4
    AURORA_TEST_CHECK_NEAR(s.height, 64.0F, 1e-4F);  // 20+40+4
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[2].bounds().origin.y, 24.0F, 1e-4F);
}

AURORA_TEST_CASE(set_columns_clamps_non_positive) {
    Grid grid;
    grid.set_columns(0);
    AURORA_TEST_CHECK_EQ(grid.columns, 1);
    grid.set_columns(-3);
    AURORA_TEST_CHECK_EQ(grid.columns, 1);
    grid.set_columns(4);
    AURORA_TEST_CHECK_EQ(grid.columns, 4);
    grid.set_gap(-1.0F);
    AURORA_TEST_CHECK_NEAR(grid.gap, -1.0F, 0.0F);  // gap 不钳制（校验走 validate 语义/描述不变量）
}

AURORA_TEST_CASE(props_serialize_deserialize_roundtrip) {
    Grid src{GridProps{.columns = 3, .gap = 8.0F}};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["columns"].get<int>(), 3);
    AURORA_TEST_CHECK_NEAR(props["gap"].get<float>(), 8.0F, 1e-6F);

    Grid dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.columns, 3);
    AURORA_TEST_CHECK_NEAR(dst.gap, 8.0F, 1e-6F);

    // 反序列化钳制：columns<=0 回落 1。
    props["columns"] = 0;
    Grid clamp;
    clamp.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(clamp.columns, 1);
}

AURORA_TEST_CASE(describe_reports_invariants) {
    const auto d = Grid::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Grid");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    AURORA_TEST_REQUIRE_EQ(d.invariants.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.invariants[0]}, "columns >= 1");
}

}  // namespace aurora::test_cases::utest_grid
