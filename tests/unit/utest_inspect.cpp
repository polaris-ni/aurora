/// 测试类型: unit
/// 目标单元: include/aurora/widget/inspect.h
/// 测试说明: 覆盖 collect_widget_boxes —— 空节点返回空表、先序遍历顺序与索引路径格式、
///           嵌套层级的路径拼装、根盒落定，以及结果与 render_to_logical_snapshot 的树同构

#include <cmath>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/inspect.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_inspect {

namespace {

/// @brief 布局一棵树（与 `render_to_logical_snapshot` 同序列：mount → layout → 落定根盒）。
auto layout_tree(Node &root, float width, float height) -> void {
    const BuildContext ctx;
    root->mount(ctx);

    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = width, .height = height};
    root->layout(c, ctx);
    // 几何唯一权威在 Node；不进 paint 的路径须自行落定根盒。
    root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = root->size()});
}

[[nodiscard]] auto make_flat_column() -> Node {
    return Node{Column(ColumnProps{
        .children = {Text(TextProps{.content = "a"}), Text(TextProps{.content = "b"})}})};
}

[[nodiscard]] auto make_nested_column() -> Node {
    return Node{Column(ColumnProps{
        .children = {Text(TextProps{.content = "a"}),
                     Column(ColumnProps{.children = {Text(TextProps{.content = "nested"})}})}})};
}

/// @brief 把逻辑快照 JSON 按同一先序铺平，用于与 collect_widget_boxes 对账。
[[nodiscard]] auto flatten_snapshot(const Json &j) -> std::vector<WidgetBox> {
    std::vector<WidgetBox> out;
    WidgetBox box;
    box.type = j["type"].get<std::string>();
    box.bounds = Rect{.origin = Point{.x = j["box"]["x"].get<float>(), .y = j["box"]["y"].get<float>()},
                      .size = Size{.width = j["box"]["w"].get<float>(), .height = j["box"]["h"].get<float>()}};
    out.push_back(box);
    for (const Json &child : j["children"]) {
        const std::vector<WidgetBox> sub = flatten_snapshot(child);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(collect_widget_boxes_empty_node_yields_empty_table) {
    const Node empty;
    AURORA_TEST_CHECK_TRUE(collect_widget_boxes(empty).empty());
}

AURORA_TEST_CASE(collect_widget_boxes_flat_tree_emits_root_then_children_with_index_paths) {
    Node root = make_flat_column();
    layout_tree(root, 200.0F, 200.0F);

    const std::vector<WidgetBox> boxes = collect_widget_boxes(root);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 3U);
    AURORA_TEST_CHECK_EQ(boxes[0].path, std::string(""));
    AURORA_TEST_CHECK_EQ(boxes[0].type, std::string("Column"));
    AURORA_TEST_CHECK_EQ(boxes[1].path, std::string("0"));
    AURORA_TEST_CHECK_EQ(boxes[1].type, std::string("Text"));
    AURORA_TEST_CHECK_EQ(boxes[2].path, std::string("1"));
    AURORA_TEST_CHECK_EQ(boxes[2].type, std::string("Text"));
}

AURORA_TEST_CASE(collect_widget_boxes_nested_paths_join_by_slash) {
    Node root = make_nested_column();
    layout_tree(root, 200.0F, 200.0F);

    const std::vector<WidgetBox> boxes = collect_widget_boxes(root);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 4U);
    // 先序：根 → 第 0 个子 → 第 1 个子 → 第 1 个子内部的第 0 个（而非广度优先）。
    AURORA_TEST_CHECK_EQ(boxes[0].path, std::string(""));
    AURORA_TEST_CHECK_EQ(boxes[0].type, std::string("Column"));
    AURORA_TEST_CHECK_EQ(boxes[1].path, std::string("0"));
    AURORA_TEST_CHECK_EQ(boxes[1].type, std::string("Text"));
    AURORA_TEST_CHECK_EQ(boxes[2].path, std::string("1"));
    AURORA_TEST_CHECK_EQ(boxes[2].type, std::string("Column"));
    AURORA_TEST_CHECK_EQ(boxes[3].path, std::string("1/0"));
    AURORA_TEST_CHECK_EQ(boxes[3].type, std::string("Text"));
}

AURORA_TEST_CASE(collect_widget_boxes_root_box_is_sealed_from_size) {
    Node root = make_flat_column();
    layout_tree(root, 200.0F, 200.0F);

    const std::vector<WidgetBox> boxes = collect_widget_boxes(root);
    AURORA_TEST_REQUIRE_FALSE(boxes.empty());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(boxes[0].bounds.origin.x), 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(boxes[0].bounds.origin.y), 0.0, 1e-6);
    AURORA_TEST_CHECK_GT(static_cast<double>(boxes[0].bounds.size.width), 0.0);
    AURORA_TEST_CHECK_GT(static_cast<double>(boxes[0].bounds.size.height), 0.0);
}

AURORA_TEST_CASE(collect_widget_boxes_children_stay_inside_parent_box) {
    Node root = make_flat_column();
    layout_tree(root, 200.0F, 200.0F);

    const std::vector<WidgetBox> boxes = collect_widget_boxes(root);
    AURORA_TEST_REQUIRE_EQ(boxes.size(), 3U);
    const Rect parent = boxes[0].bounds;
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        AURORA_TEST_CHECK_GE(static_cast<double>(boxes[i].bounds.origin.y),
                             static_cast<double>(parent.origin.y) - 1e-6);
        AURORA_TEST_CHECK_LE(static_cast<double>(boxes[i].bounds.bottom()),
                             static_cast<double>(parent.bottom()) + 1e-6);
    }
}

AURORA_TEST_CASE(diff_trees_empty_when_nothing_changed) {
    Node before = make_nested_column();
    layout_tree(before, 200.0F, 200.0F);
    Node after = make_nested_column();
    layout_tree(after, 200.0F, 200.0F);

    // 同构同值 ⇒ 无补丁；这也顺带说明 diff 不会因为布局/环境噪声而误报。
    AURORA_TEST_CHECK_TRUE(diff_trees(before, after).empty());
    AURORA_TEST_CHECK_FALSE(trees_differ_structurally(before, after));
}

AURORA_TEST_CASE(diff_trees_reports_changed_props_in_inspector_path_format) {
    // 只改一个文本：补丁应当只有少数几条，且路径是 Inspector / REST 认得的 "/<索引路径>/<属性名>"。
    Node before = Node{Column(ColumnProps{
        .children = {Text(TextProps{.content = "before"}), Text(TextProps{.content = "same"})}})};
    layout_tree(before, 200.0F, 200.0F);
    Node after = Node{Column(ColumnProps{
        .children = {Text(TextProps{.content = "after"}), Text(TextProps{.content = "same"})}})};
    layout_tree(after, 200.0F, 200.0F);

    const std::vector<WidgetPatchOp> ops = diff_trees(before, after);
    AURORA_TEST_REQUIRE_FALSE(ops.empty());

    bool found_content = false;
    for (const WidgetPatchOp &op : ops) {
        AURORA_TEST_CHECK_EQ(op.op, std::string("replace"));
        AURORA_TEST_CHECK_EQ(op.path.front(), '/');
        if (op.path == "/0/content") {
            found_content = true;
            AURORA_TEST_CHECK_EQ(op.value.get<std::string>(), std::string("after"));
        }
    }
    AURORA_TEST_CHECK_TRUE(found_content);
    AURORA_TEST_CHECK_FALSE(trees_differ_structurally(before, after));
}

AURORA_TEST_CASE(diff_trees_tolerates_structural_difference_but_flags_it) {
    // 子节点数不同 ⇒ 结构性差异，属性补丁无法表达；补丁仍然给出（对可比较的部分），
    // 但调用方必须据此改走整树替换。
    Node before = Node{Column(ColumnProps{.children = {Text(TextProps{.content = "a"})}})};
    layout_tree(before, 200.0F, 200.0F);
    Node after = Node{Column(ColumnProps{
        .children = {Text(TextProps{.content = "a"}), Text(TextProps{.content = "b"})}})};
    layout_tree(after, 200.0F, 200.0F);

    AURORA_TEST_CHECK_TRUE(trees_differ_structurally(before, after));
    // diff 只比较公共前缀（此处第 0 个子节点完全相同），不越界读缺失的一侧 ⇒ 无补丁。
    AURORA_TEST_CHECK_TRUE(diff_trees(before, after).empty());
}

AURORA_TEST_CASE(diff_trees_handles_empty_nodes) {
    const Node empty;
    Node real = make_flat_column();
    layout_tree(real, 200.0F, 200.0F);

    AURORA_TEST_CHECK_TRUE(diff_trees(empty, real).empty());
    AURORA_TEST_CHECK_TRUE(diff_trees(real, empty).empty());
    // 一方为空属结构性差异。
    AURORA_TEST_CHECK_TRUE(trees_differ_structurally(empty, real));
}

AURORA_TEST_CASE(collect_widget_boxes_agrees_with_offscreen_logical_snapshot) {
    // 交叉校验：另一条独立通道（offscreen 的递归 JSON 快照）铺平后应逐项一致 ——
    // 类型、几何都要对齐，因为归因结果的可信度完全建立在这份几何上。
    Node root = make_nested_column();
    layout_tree(root, 200.0F, 200.0F);

    const Json snapshot = render_to_logical_snapshot(root, 200, 200);
    const std::vector<WidgetBox> expected = flatten_snapshot(snapshot);
    const std::vector<WidgetBox> got = collect_widget_boxes(root);

    AURORA_TEST_REQUIRE_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        AURORA_TEST_CHECK_EQ(got[i].type, expected[i].type);
        AURORA_TEST_CHECK_NEAR(static_cast<double>(got[i].bounds.origin.x),
                               static_cast<double>(expected[i].bounds.origin.x), 1e-3);
        AURORA_TEST_CHECK_NEAR(static_cast<double>(got[i].bounds.origin.y),
                               static_cast<double>(expected[i].bounds.origin.y), 1e-3);
        AURORA_TEST_CHECK_NEAR(static_cast<double>(got[i].bounds.size.width),
                               static_cast<double>(expected[i].bounds.size.width), 1e-3);
        AURORA_TEST_CHECK_NEAR(static_cast<double>(got[i].bounds.size.height),
                               static_cast<double>(expected[i].bounds.size.height), 1e-3);
    }
}

}  // namespace aurora::test_cases::utest_inspect
