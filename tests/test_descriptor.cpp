// test_descriptor.cpp — 控件自描述 describe() 与组件发现 API 增强测试（v0.7.0）。

#include <string>
#include <vector>

#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/show.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::Button;
using aurora::Column;
using aurora::describe_component;
using aurora::Json;
using aurora::list_all_components;
using aurora::list_all_schemas;
using aurora::Row;
using aurora::Scroll;
using aurora::search_components;
using aurora::Show;
using aurora::Text;
using aurora::Widget;
using aurora::serialization::component_schema;
using aurora::serialization::register_core_widgets;

// ---------- describe() 基本正确性 ----------

static void test_describe_basic() {
    // Button
    auto bd = Button::describe_static();
    AURORA_TEST_CHECK(bd.name == "Button");
    AURORA_TEST_CHECK(bd.ns == "aurora");
    AURORA_TEST_CHECK(!bd.properties.empty());
    AURORA_TEST_CHECK(bd.children_policy == "none");
    AURORA_TEST_CHECK(!bd.events.empty());
    AURORA_TEST_CHECK(bd.events[0] == "on_click");
    AURORA_TEST_CHECK(!bd.examples.empty());

    // Button label 属性应为 required
    bool found_label = false;
    for (const auto &p : bd.properties) {
        if (p.name == "label") {
            found_label = true;
            AURORA_TEST_CHECK(p.required);
            AURORA_TEST_CHECK(p.type == "LocalizedString");
        }
    }
    AURORA_TEST_CHECK(found_label);

    // Text
    auto td = Text::describe_static();
    AURORA_TEST_CHECK(td.name == "Text");
    AURORA_TEST_CHECK(td.children_policy == "none");
    bool found_content = false;
    for (const auto &p : td.properties) {
        if (p.name == "content") {
            found_content = true;
            AURORA_TEST_CHECK(p.required);
        }
    }
    AURORA_TEST_CHECK(found_content);

    // Column（容器）
    auto cd = Column::describe_static();
    AURORA_TEST_CHECK(cd.name == "Column");
    AURORA_TEST_CHECK(cd.children_policy == "multiple");

    // Row
    auto rd = Row::describe_static();
    AURORA_TEST_CHECK(rd.name == "Row");
    AURORA_TEST_CHECK(rd.children_policy == "multiple");

    // Scroll（单子容器）
    auto sd = Scroll::describe_static();
    AURORA_TEST_CHECK(sd.name == "Scroll");
    AURORA_TEST_CHECK(sd.children_policy == "single");

    // Show（单子容器）
    auto shd = Show::describe_static();
    AURORA_TEST_CHECK(shd.name == "Show");
    AURORA_TEST_CHECK(shd.children_policy == "single");
}

// ---------- describe() 虚函数多态调用 ----------

static void test_describe_virtual() {
    Button btn;
    Widget &w = btn;
    auto desc = w.describe();
    AURORA_TEST_CHECK(desc.name == "Button");
    AURORA_TEST_CHECK(desc.name == btn.type_name());

    Text txt("hello");
    Widget &w2 = txt;
    auto desc2 = w2.describe();
    AURORA_TEST_CHECK(desc2.name == "Text");
    AURORA_TEST_CHECK(desc2.name == txt.type_name());

    Column col;
    Widget &w3 = col;
    auto desc3 = w3.describe();
    AURORA_TEST_CHECK(desc3.name == "Column");
}

// ---------- descriptor_to_json 序列化 ----------

static void test_descriptor_to_json() {
    const auto bd = Button::describe_static();
    Json j = descriptor_to_json(bd);

    AURORA_TEST_CHECK(j.contains("name"));
    AURORA_TEST_CHECK(j["name"] == "Button");
    AURORA_TEST_CHECK(j.contains("namespace"));
    AURORA_TEST_CHECK(j["namespace"] == "aurora");
    AURORA_TEST_CHECK(j.contains("properties"));
    AURORA_TEST_CHECK(j["properties"].is_array());
    AURORA_TEST_CHECK(!j["properties"].empty());
    AURORA_TEST_CHECK(j.contains("events"));
    AURORA_TEST_CHECK(j["events"].is_array());
    AURORA_TEST_CHECK(j.contains("children_policy"));
    AURORA_TEST_CHECK(j["children_policy"] == "none");
    AURORA_TEST_CHECK(j.contains("examples"));
    AURORA_TEST_CHECK(j["examples"].is_array());

    // 检查单个属性结构
    const Json first_prop = j["properties"][0];
    AURORA_TEST_CHECK(first_prop.contains("name"));
    AURORA_TEST_CHECK(first_prop.contains("type"));
    AURORA_TEST_CHECK(first_prop.contains("default"));
    AURORA_TEST_CHECK(first_prop.contains("required"));
}

// ---------- component_schema 增强字段 ----------

static void test_component_schema_enhanced() {
    register_core_widgets();

    Json schema = component_schema("Button");
    AURORA_TEST_CHECK(schema.contains("prop_descriptors"));
    AURORA_TEST_CHECK(schema["prop_descriptors"].is_array());
    AURORA_TEST_CHECK(!schema["prop_descriptors"].empty());
    AURORA_TEST_CHECK(schema.contains("events"));
    AURORA_TEST_CHECK(schema["events"].is_array());
    AURORA_TEST_CHECK(schema.contains("children_policy"));
    AURORA_TEST_CHECK(schema["children_policy"] == "none");
    AURORA_TEST_CHECK(schema.contains("examples"));
    AURORA_TEST_CHECK(schema["examples"].is_array());

    // 向后兼容：原有字段仍存在
    AURORA_TEST_CHECK(schema.contains("type"));
    AURORA_TEST_CHECK(schema["type"] == "Button");
    AURORA_TEST_CHECK(schema.contains("props"));
    AURORA_TEST_CHECK(schema["props"].is_array());
    AURORA_TEST_CHECK(schema.contains("default_props"));
    AURORA_TEST_CHECK(schema.contains("container"));

    // Column 的 children_policy 应为 multiple
    Json col_schema = component_schema("Column");
    AURORA_TEST_CHECK(col_schema["children_policy"] == "multiple");
}

// ---------- list_all_schemas ----------

static void test_list_all_schemas() {
    register_core_widgets();

    const auto types = list_all_components();
    const auto schemas = list_all_schemas();
    AURORA_TEST_CHECK(schemas.size() == types.size());
    AURORA_TEST_CHECK(!schemas.empty());

    // 每个 schema 都应有 type 和 prop_descriptors 字段
    for (const auto &s : schemas) {
        AURORA_TEST_CHECK(s.contains("type"));
        AURORA_TEST_CHECK(s.contains("prop_descriptors"));
        AURORA_TEST_CHECK(s.contains("children_policy"));
    }
}

// ---------- describe_component 公共 API ----------

static void test_describe_component_api() {
    Json desc = describe_component("Text");
    AURORA_TEST_CHECK(desc.contains("prop_descriptors"));
    AURORA_TEST_CHECK(desc.contains("events"));
    AURORA_TEST_CHECK(desc.contains("children_policy"));
    AURORA_TEST_CHECK(desc["children_policy"] == "none");

    // 搜索
    const auto results = search_components("but");
    AURORA_TEST_CHECK(!results.empty());
    bool found_button = false;
    for (const auto &r : results) {
        if (r["type"] == "Button") {
            found_button = true;
        }
    }
    AURORA_TEST_CHECK(found_button);
}

AURORA_TEST() {
    test_describe_basic();
    test_describe_virtual();
    test_descriptor_to_json();
    test_component_schema_enhanced();
    test_list_all_schemas();
    test_describe_component_api();
}
