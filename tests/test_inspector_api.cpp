// test_inspector_api.cpp — Inspector 统一门面测试。

#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/inspector/inspector_api.h"
#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::Inspector;
using aurora::Json;
using aurora::Node;
using aurora::Row;
using aurora::RowProps;
using aurora::Size;
using aurora::Text;
using aurora::TextInput;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_inspector_api ===\n");

    serialization::register_core_widgets();

    // 构建测试树：Column → [Button, Text, Row → [Text]]
    auto root = Node{Column{ColumnProps{.children = {
                                            Node{Button{"OK"}},
                                            Node{Text{"label"}},
                                            Node{Row{RowProps{.children =
                                                                  {
                                                                      Node{Text{"inner"}},
                                                                  }}}},
                                        }}}};

    BuildContext ctx;
    root.widget().mount(ctx);
    root.widget().layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 400, .height = 400}},
                         ctx);

    // ---- 1) tree_json 基本结构 ----
    {
        Json j = Inspector::tree_json(root);
        AURORA_TEST_CHECK(j.is_object());
        AURORA_TEST_CHECK(j.contains("type"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["type"].get<std::string>() == "Column");
        AURORA_TEST_CHECK(j.contains("children"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["children"].is_array());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_EQ(j["children"].size(), 3U);
    }

    // ---- 2) tree_rich 包含 bounds/text 信息 ----
    {
        std::string rich = Inspector::tree_rich(root);
        AURORA_TEST_CHECK(!rich.empty());
        // 应包含 Column 类型名
        AURORA_TEST_CHECK(rich.find("Column") != std::string::npos);
        // 应包含 Button 类型名
        AURORA_TEST_CHECK(rich.find("Button") != std::string::npos);
    }

    // ---- 3) tree_text 人类可读 ----
    {
        std::string text = Inspector::tree_text(root);
        AURORA_TEST_CHECK(!text.empty());
        AURORA_TEST_CHECK(text.find("Column") != std::string::npos);
    }

    // ---- 4) tree_json_full 含 props ----
    {
        Json j = Inspector::tree_json_full(root);
        AURORA_TEST_CHECK(j.contains("props"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(j["props"].is_object());
    }

    // ---- 5) query 按类型查询 ----
    {
        auto texts = Inspector::query("Text", root);
        AURORA_TEST_CHECK_EQ(texts.size(), 2U);  // "label" + "inner"
        auto buttons = Inspector::query("Button", root);
        AURORA_TEST_CHECK_EQ(buttons.size(), 1U);
        auto missing = Inspector::query("NonExistent", root);
        AURORA_TEST_CHECK_EQ(missing.size(), 0U);
    }

    // ---- 6) get_state 路径查询 ----
    {
        Json state = Inspector::get_state("type", root);
        AURORA_TEST_CHECK(state.is_string());
        AURORA_TEST_CHECK(state.get<std::string>() == "Column");
    }

    // ---- 7) find_node 路径定位 ----
    {
        Node n0 = Inspector::find_node(root, "");
        AURORA_TEST_CHECK(static_cast<bool>(n0));
        AURORA_TEST_CHECK(std::string(n0.widget().type_name()) == "Column");

        Node n1 = Inspector::find_node(root, "0");
        AURORA_TEST_CHECK(static_cast<bool>(n1));
        AURORA_TEST_CHECK(std::string(n1.widget().type_name()) == "Button");

        Node n2 = Inspector::find_node(root, "2/0");
        AURORA_TEST_CHECK(static_cast<bool>(n2));
        AURORA_TEST_CHECK(std::string(n2.widget().type_name()) == "Text");

        Node invalid = Inspector::find_node(root, "99");
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(invalid));
    }

    // ---- 8) get_prop / get_prop_value ----
    {
        Json props = Inspector::get_prop(root.widget());
        AURORA_TEST_CHECK(props.is_object());
        AURORA_TEST_CHECK(props.contains("descriptor"));
        AURORA_TEST_CHECK(props.contains("values"));

        Json gap_val = Inspector::get_prop_value(root.widget(), "gap");
        // gap 应该有值（可能是默认值）
        AURORA_TEST_CHECK(!gap_val.is_null());
    }

    // ---- 9) set_prop 属性回写 ----
    {
        auto *col = dynamic_cast<Column *>(&root.widget());
        AURORA_TEST_CHECK(col != nullptr);
        float old_gap = col->gap;

        auto result = Inspector::set_prop(root.widget(), "gap", Json(20.0F));
        AURORA_TEST_CHECK(static_cast<bool>(result));
        AURORA_TEST_CHECK_NEAR(col->gap, 20.0F, 0.01F);

        // 恢复
        Inspector::set_prop(root.widget(), "gap", Json(old_gap));
    }

    // ---- 10) widget_info ----
    {
        Json info = Inspector::widget_info(root.widget());
        AURORA_TEST_CHECK(info.is_object());
        AURORA_TEST_CHECK(info.contains("descriptor"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(info["descriptor"]["name"].get<std::string>() == "Column");
    }

    // ---- 11) components 组件发现 ----
    {
        auto comps = Inspector::components();
        AURORA_TEST_CHECK(!comps.empty());
        // 应包含核心组件
        bool has_column = false;
        for (const auto &c : comps) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (c.contains("type") && c["type"].get<std::string>() == "Column") {
                has_column = true;
                break;
            }
        }
        AURORA_TEST_CHECK(has_column);
    }

    // ---- 12) component_schema ----
    {
        Json schema = Inspector::component_schema("Button");
        AURORA_TEST_CHECK(schema.is_object());
        // schema 应含 type 或 name 字段
        AURORA_TEST_CHECK(schema.contains("type") || schema.contains("name"));
    }

    // ---- 13) to_code 代码生成 ----
    {
        std::string code = Inspector::to_code(root);
        AURORA_TEST_CHECK(!code.empty());
        AURORA_TEST_CHECK(code.find("Column") != std::string::npos);
    }

    // ---- 14) validate 验证 ----
    {
        auto diags = Inspector::validate(root);
        // 合法树应该没有诊断
        AURORA_TEST_CHECK(diags.empty());
    }

    // ---- 15) subscribe_changes / notify_changes / unsubscribe ----
    {
        int call_count = 0;
        Json last_patch;

        auto id = Inspector::subscribe_changes([&](const Json &patch) -> void {
            ++call_count;
            last_patch = patch;
        });
        AURORA_TEST_CHECK(id > 0);

        // 触发通知
        Json test_patch = Json::object();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        test_patch["test"] = "value";
        Inspector::notify_changes(test_patch);

        AURORA_TEST_CHECK_EQ(call_count, 1);
        AURORA_TEST_CHECK(last_patch.contains("test"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(last_patch["test"].get<std::string>() == "value");

        // 取消订阅后再通知不应增加计数
        Inspector::unsubscribe(id);
        Inspector::notify_changes(test_patch);
        AURORA_TEST_CHECK_EQ(call_count, 1);
    }

    // ---- 16) simulate_* 真实派发事件（不再返回 GeneralNotSupported）----
    {
        BuildContext lctx;

        // click：在 Button 上派发 press+release，返回成功（不再为 stub）
        Button btn{"test"};
        btn.mount(lctx);
        btn.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 60}}, lctx);
        auto r1 = Inspector::simulate_click(btn);
        AURORA_TEST_CHECK(static_cast<bool>(r1));

        // scroll：在 Column 上派发 WheelEvent，返回成功
        Column col;
        col.mount(lctx);
        col.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 200}}, lctx);
        auto r2 = Inspector::simulate_scroll(col, 0.0F, 12.0F);
        AURORA_TEST_CHECK(static_cast<bool>(r2));

        // text_input：焦点派发到 TextInput，文本被真实写入
        TextInput ti;
        ti.mount(lctx);
        ti.layout(Constraints{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 200, .height = 40}}, lctx);
        AURORA_TEST_CHECK_EQ(ti.value(), "");
        auto r3 = Inspector::simulate_text_input(ti, "hello");
        AURORA_TEST_CHECK(static_cast<bool>(r3));
        AURORA_TEST_CHECK_EQ(ti.value(), "hello");
    }
}