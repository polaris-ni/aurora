// test_inspector_panel.cpp — InspectorPanel 与 inspect.h 扩展函数测试。

#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::InspectorPanel;
using aurora::Json;
using aurora::Node;
using aurora::Row;
using aurora::RowProps;
using aurora::Size;
using aurora::Text;
using aurora::TextInput;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_inspector_panel ===\n");

    // 构建测试树：Column → [Button, Text, Row → [Text, TextInput]]
    TextInput ti;
    ti.set_value("hello");
    auto root = Node{ Column{ ColumnProps{ .children = {
        Node{ Button{ "OK" } },
        Node{ Text{ "label" } },
        Node{ Row{ RowProps{ .children = {
            Node{ Text{ "inner" } },
            Node{ std::move(ti) },
        } } } },
    } } } };

    BuildContext ctx;
    root.widget().mount(ctx);
    root.widget().layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 400, .height = 400 } }, ctx);

    // ---- 1) widget_tree_to_items 转换正确性 ----
    {
        auto items = widget_tree_to_items(root);
        AURORA_TEST_CHECK_EQ(items.size(), 1u); // 一个根节点
        AURORA_TEST_CHECK(items[0].label == "Column");
        AURORA_TEST_CHECK(items[0].expanded);               // 根默认展开
        AURORA_TEST_CHECK_EQ(items[0].children.size(), 3u); // Button, Text, Row
        AURORA_TEST_CHECK(items[0].children[0].label == "Button");
        AURORA_TEST_CHECK(items[0].children[1].label == "Text");
        AURORA_TEST_CHECK(items[0].children[2].label == "Row");
        AURORA_TEST_CHECK_EQ(items[0].children[2].children.size(), 2u); // Text, TextInput
        AURORA_TEST_CHECK(items[0].children[2].children[0].label == "Text");
        AURORA_TEST_CHECK(items[0].children[2].children[1].label == "TextInput");
    }

    // ---- 2) dump_tree_json_full 包含 props ----
    {
        auto j = dump_tree_json_full(root);
        AURORA_TEST_CHECK(j.is_object());
        AURORA_TEST_CHECK(j.contains("type"));
        AURORA_TEST_CHECK(j["type"].get<std::string>() == "Column");
        AURORA_TEST_CHECK(j.contains("props"));
        AURORA_TEST_CHECK(j["props"].is_object());
        AURORA_TEST_CHECK(j.contains("children"));
        AURORA_TEST_CHECK(j["children"].is_array());
        AURORA_TEST_CHECK_EQ(j["children"].size(), 3u);
        // 子节点也含 props
        AURORA_TEST_CHECK(j["children"][0].contains("props"));
        AURORA_TEST_CHECK(j["children"][0]["type"].get<std::string>() == "Button");
    }

    // ---- 3) find_node_by_path 路径定位 ----
    {
        // 根节点
        auto n0 = find_node_by_path(root, "");
        AURORA_TEST_CHECK(static_cast<bool>(n0));
        AURORA_TEST_CHECK(std::string(n0.widget().type_name()) == "Column");

        // 第一个子节点 = Button
        auto n1 = find_node_by_path(root, "0");
        AURORA_TEST_CHECK(static_cast<bool>(n1));
        AURORA_TEST_CHECK(std::string(n1.widget().type_name()) == "Button");

        // 第二个子节点 = Text
        auto n2 = find_node_by_path(root, "1");
        AURORA_TEST_CHECK(static_cast<bool>(n2));
        AURORA_TEST_CHECK(std::string(n2.widget().type_name()) == "Text");

        // Row 下的 TextInput = "2/1"
        auto n3 = find_node_by_path(root, "2/1");
        AURORA_TEST_CHECK(static_cast<bool>(n3));
        AURORA_TEST_CHECK(std::string(n3.widget().type_name()) == "TextInput");

        // 越界路径返回空 Node
        auto n4 = find_node_by_path(root, "99");
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(n4));

        auto n5 = find_node_by_path(root, "2/5");
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(n5));
    }

    // ---- 4) get_widget_props 属性快照 ----
    {
        auto props = get_widget_props(root.widget());
        AURORA_TEST_CHECK(props.is_object());
        AURORA_TEST_CHECK(props.contains("descriptor"));
        AURORA_TEST_CHECK(props["descriptor"]["name"].get<std::string>() == "Column");
        AURORA_TEST_CHECK(props.contains("values"));
        AURORA_TEST_CHECK(props["values"].is_object());
        // Column 有 gap 属性
        AURORA_TEST_CHECK(props["values"].contains("gap"));
    }

    // ---- 5) set_widget_prop 属性回写 ----
    {
        // 修改 Column 的 gap
        auto before = root.widget().child_nodes();
        auto *col = dynamic_cast<Column *>(&root.widget());
        AURORA_TEST_CHECK(col != nullptr);
        float old_gap = col->gap;

        set_widget_prop(root.widget(), "gap", Json(16.0f));
        AURORA_TEST_CHECK_NEAR(col->gap, 16.0f, 0.01f);

        // 恢复
        set_widget_prop(root.widget(), "gap", Json(old_gap));
        AURORA_TEST_CHECK_NEAR(col->gap, old_gap, 0.01f);
    }

    // ---- 6) InspectorPanel 创建和基本操作 ----
    {
        InspectorPanel panel{ [&]() -> Node {
            return Node{ Column{ ColumnProps{ .children = {
                                                  Node{ Text{ "A" } },
                                                  Node{ Button{ "B" } },
                                              } } } };
        } };

        AURORA_TEST_CHECK(std::string(panel.type_name()) == "InspectorPanel");
        AURORA_TEST_CHECK(panel.selected_widget() == nullptr);

        // refresh 不崩溃
        panel.refresh();

        // describe 返回正确名称
        auto desc = panel.describe();
        AURORA_TEST_CHECK(desc.name == "InspectorPanel");

        // serialize_props 含 ratio
        Json props = Json::object();
        panel.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("ratio"));
    }

    // ---- 7) widget_tree_to_items 空树 ----
    {
        auto empty_leaf = Node{ Text{ "solo" } };
        auto items = widget_tree_to_items(empty_leaf);
        AURORA_TEST_CHECK_EQ(items.size(), 1u);
        AURORA_TEST_CHECK(items[0].label == "Text");
        AURORA_TEST_CHECK(items[0].children.empty());
    }

    // ---- 8) dump_tree_json_full 叶节点无 children 数组为空 ----
    {
        auto leaf = Node{ Button{ "x" } };
        auto j = dump_tree_json_full(leaf);
        AURORA_TEST_CHECK(j["children"].is_array());
        AURORA_TEST_CHECK_EQ(j["children"].size(), 0u);
    }
}
