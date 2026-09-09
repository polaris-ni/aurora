/// 测试类型: unit
/// 目标单元: include/aurora/layout/layout_engine.h
/// 测试说明: 覆盖布局引擎三入口——layout 驱动父控件经 Node::set_bounds 落定子盒、layout_to_box 产出
/// LayoutBox 树、build_box 由已布局树收集几何（几何权威在 Node::bounds_，根由调用方定位）

#include <memory>

#include "aurora/aurora.h"
#include "aurora/layout/layout_engine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_layout_engine {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(px(w));
    t->height(px(h));
    return Node{t};
}

auto column_of_two() -> Node {
    auto col = std::make_shared<Column>();
    col->add(box(100.0F, 20.0F));
    col->add(box(100.0F, 20.0F));
    return Node{col};
}

auto fixed_constraints(float max_w = 300.0F, float max_h = 100.0F) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
}

}  // namespace

AURORA_TEST_CASE(layout_places_children_bounds_via_parent) {
    // 几何权威收敛在 Node::bounds_：布局后子盒由父控件（Column）写入，含真实 origin。
    Node root = column_of_two();
    LayoutEngine::layout(root.widget(), fixed_constraints());

    const auto& kids = root.widget().child_nodes();
    AURORA_TEST_REQUIRE_EQ(kids.size(), 2U);
    const Rect& first = kids[0].bounds();
    const Rect& second = kids[1].bounds();
    AURORA_TEST_CHECK_NEAR(first.origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(first.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(first.size.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(second.origin.y, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(second.size.height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_to_box_produces_geometry_tree) {
    Node root = column_of_two();
    // 根节点没有父级：渲染管线由调用方对根定位（此处模拟之），子级由布局写入。
    root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});

    const LayoutBox tree = LayoutEngine::layout_to_box(root, fixed_constraints());

    AURORA_TEST_CHECK_NEAR(tree.rect.size.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.rect.size.height, 40.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(tree.children[0].rect.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.children[1].rect.origin.y, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.children[1].rect.size.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(tree.children[0].children.empty());
}

AURORA_TEST_CASE(build_box_collects_after_layout_without_relayout) {
    auto col = std::make_shared<Column>();
    col->add(box(80.0F, 30.0F));
    Node root{col};

    LayoutEngine::layout(root.widget(), fixed_constraints());
    root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 80.0F, .height = 30.0F}});
    const LayoutBox tree = LayoutEngine::build_box(root);

    // build_box 只收集不布局：几何与布局结果一致。
    AURORA_TEST_CHECK_NEAR(tree.rect.size.width, 80.0F, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 1U);
    AURORA_TEST_CHECK_NEAR(tree.children[0].rect.size.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.children[0].rect.origin.y, 0.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_layout_engine
