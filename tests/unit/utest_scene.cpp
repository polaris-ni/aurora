/// 测试类型: unit
/// 目标单元: include/aurora/app/scene.h
/// 测试说明: 覆盖 Scene 持有 widget 树的构造与根访问（root/root_node 一致性）、
/// 结构快照 serialize 的 JSON 形态（叶/嵌套树/空容器无 children 键）及其随布局尺寸更新

#include <memory>
#include <string>

#include "aurora/app/scene.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scene {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto count_of(const std::string& haystack, const std::string& needle) -> std::size_t {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

AURORA_TEST_CASE(scene_holds_root_widget) {
    Scene scene{Node{std::make_shared<Text>("hi")}};

    // root() 返回根 widget；root_node() 与之指向同一实例。
    AURORA_TEST_CHECK_EQ(std::string{scene.root().type_name()}, "Text");
    AURORA_TEST_CHECK_EQ(&scene.root(), &scene.root_node().widget());
    AURORA_TEST_CHECK_EQ(&scene.root(), &scene.root());
}

AURORA_TEST_CASE(serialize_leaf_json_shape) {
    Scene scene{Node{std::make_shared<Text>("solo")}};

    const std::string out = scene.serialize();
    // 叶子节点：type + size，无 children 键。
    AURORA_TEST_CHECK_TRUE(out.starts_with(R"({"type":"Text")"));
    AURORA_TEST_CHECK_TRUE(out.find(R"("size":[)") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(out.ends_with('}'));
    AURORA_TEST_CHECK_TRUE(out.find("\"children\"") == std::string::npos);
}

AURORA_TEST_CASE(serialize_nested_tree_lists_children_in_order) {
    auto row = std::make_shared<Row>();
    row->add(box(10.0F, 10.0F));
    row->add(box(20.0F, 10.0F));
    Scene scene{Node{row}};

    const std::string out = scene.serialize();
    // 根为 Row，两个 Text 子项按注册顺序出现。
    AURORA_TEST_CHECK_TRUE(out.starts_with(R"({"type":"Row")"));
    AURORA_TEST_CHECK_TRUE(out.find("\"children\"") != std::string::npos);
    AURORA_TEST_CHECK_EQ(count_of(out, R"("type":"Text")"), 2U);

    // 嵌套：Column → Row → Text 的深度与顺序。
    auto inner = std::make_shared<Row>();
    inner->add(box(8.0F, 8.0F));
    auto outer = std::make_shared<Column>();
    outer->add(Node{inner});
    Scene nested{Node{outer}};

    const std::string nested_out = nested.serialize();
    const auto pos_column = nested_out.find(R"("type":"Column")");
    const auto pos_row = nested_out.find(R"("type":"Row")");
    const auto pos_text = nested_out.find(R"("type":"Text")");
    AURORA_TEST_CHECK_NE(pos_column, std::string::npos);
    AURORA_TEST_CHECK_NE(pos_row, std::string::npos);
    AURORA_TEST_CHECK_NE(pos_text, std::string::npos);
    AURORA_TEST_CHECK_TRUE(pos_column < pos_row);
    AURORA_TEST_CHECK_TRUE(pos_row < pos_text);
}

AURORA_TEST_CASE(serialize_reflects_layout_sizes) {
    auto row = std::make_shared<Row>();
    row->add(box(10.0F, 20.0F));
    Scene scene{Node{row}};

    // 布局前：尺寸均为占位 0。
    std::string before = scene.serialize();
    AURORA_TEST_CHECK_NE(before.find(R"("size":[0.0,0.0])"), std::string::npos);

    // 布局后：根与子项的快照尺寸同步为实测值（Min 主轴下 Row 收缩到子项 10x20）。
    LayoutEngine::layout(scene.root(), bounded(200.0F, 200.0F));
    const std::string out = scene.serialize();
    AURORA_TEST_CHECK_NE(out.find(R"("size":[10.0,20.0])"), std::string::npos);
    AURORA_TEST_CHECK_TRUE(out.find(R"("size":[0.0,0.0])") == std::string::npos);
}

AURORA_TEST_CASE(serialize_empty_container_has_no_children_key) {
    Scene scene{Node{std::make_shared<Row>()}};

    const std::string out = scene.serialize();
    AURORA_TEST_CHECK_TRUE(out.starts_with(R"({"type":"Row")"));
    AURORA_TEST_CHECK_TRUE(out.find("\"children\"") == std::string::npos);
}

AURORA_TEST_CASE(serialize_snapshot_is_repeatable_and_pure) {
    auto row = std::make_shared<Row>();
    row->add(box(12.0F, 12.0F));
    Scene scene{Node{row}};

    LayoutEngine::layout(scene.root(), bounded(100.0F, 100.0F));
    const std::string first = scene.serialize();
    const std::string second = scene.serialize();
    // serialize 为 const 纯读取：两次快照逐字一致。
    AURORA_TEST_CHECK_EQ(first, second);
    AURORA_TEST_CHECK_NE(first.find(R"("size":[12.0,12.0])"), std::string::npos);
}

}  // namespace aurora::test_cases::utest_scene
