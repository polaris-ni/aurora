// 目标源单元：widget/serialization.h + widget/codegen.h + widget/yaml.h + src/aurora/widget/serialization.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_serialization.cpp
//   - test_codegen.cpp
//   - test_codegen_extended.cpp
//   - test_to_yaml.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// widget/codegen.h(to_code 三风格代码生成)、widget/yaml.h(to_yaml 发射器)。

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/yaml.h"

#include "test_harness.h"

using aurora::Alignment;
using aurora::Button;
using aurora::Column;
using aurora::describe_component;
using aurora::Grid;
using aurora::Hero;
using aurora::Json;
using aurora::list_all_components;
using aurora::Node;
using aurora::Row;
using aurora::Scroll;
using aurora::search_components;
using aurora::Stack;
using aurora::StackFit;
using aurora::Text;
using aurora::serialization::CodeStyle;
using aurora::serialization::diff;
using aurora::serialization::from_json;
using aurora::serialization::JsonPatchOp;
using aurora::serialization::register_core_widgets;
using aurora::serialization::to_code;
using aurora::serialization::to_json;
using aurora::serialization::to_yaml;
using aurora::serialization::WidgetRegistry;

namespace aurora::tests::sec_serialization {

// ---- to_json 结构 ----
static void test_to_json() {
    Node root = Column{ Text{ "Hello" }, Text{ "World" } };
    Json j = to_json(root.widget());
    AURORA_TEST_CHECK_MSG(j["type"] == "Column", "to_json: root type Column");
    AURORA_TEST_CHECK_MSG(j.contains("children") && j["children"].size() == 2, "to_json: two children");
    AURORA_TEST_CHECK_MSG(j["children"][0]["type"] == "Text", "to_json: child[0] is Text");
    AURORA_TEST_CHECK_MSG(j["children"][0]["props"]["content"] == "Hello", "to_json: child content serialized");

    const Text t{ "Hi" };
    Json jt = to_json(t);
    AURORA_TEST_CHECK_MSG(jt["type"] == "Text", "to_json: Text type");
    AURORA_TEST_CHECK_MSG(jt["props"]["content"] == "Hi", "to_json: Text content prop");
}

// ---- from_json 往返 ----
static void test_round_trip() {
    Node root = Column{ Text{ "Hello" }, Row{ Button{ "Click" }, Text{ "World" } } };
    Json j = to_json(root.widget());

    auto back = from_json(j);
    AURORA_TEST_CHECK_MSG(back.ok(), "from_json: rebuilds tree");
    if (back.ok()) {
        Json j2 = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(j == j2, "serialization: round-trip produces equal JSON");
    }

    // 单叶控件往返
    Text t{ "Hi" };
    Json jt = to_json(t);
    auto tb = from_json(jt);
    AURORA_TEST_CHECK_MSG(tb.ok() && tb.value()->type_name() == std::string("Text"), "from_json: Text rebuilt");
    if (tb.ok()) {
        Json jt2 = to_json(*tb.value());
        AURORA_TEST_CHECK_MSG(jt == jt2, "serialization: Text round-trip equal");
    }
}

// ---- WidgetRegistry ----
static void test_registry() {
    register_core_widgets();
    auto txt = WidgetRegistry::instance().make("Text", Json::object());
    AURORA_TEST_CHECK_MSG(txt.ok(), "WidgetRegistry: make Text ok");
    AURORA_TEST_CHECK_MSG(txt.value()->type_name() == std::string("Text"), "WidgetRegistry: Text instance");

    auto unknown = WidgetRegistry::instance().make("DoesNotExist", Json::object());
    AURORA_TEST_CHECK_MSG(!unknown.ok(), "WidgetRegistry: unknown type errors");
    AURORA_TEST_CHECK_MSG(unknown.error().code == "widget-unknown-type", "WidgetRegistry: unknown error code");

    auto types = WidgetRegistry::instance().list_types();
    AURORA_TEST_CHECK_MSG(!types.empty(), "WidgetRegistry: list_types non-empty");
    bool has_core = false;
    for (const auto &t : types) {
        if (t == "Text" || t == "Button" || t == "Column" || t == "Row") {
            has_core = true;
        }
    }
    AURORA_TEST_CHECK_MSG(has_core, "WidgetRegistry: lists core widgets");
}

// ---- 不可重建控件（预期错误路径） ----
static void test_non_restorable() {
    auto canvas = from_json(Json{ { "type", "Canvas" } });
    AURORA_TEST_CHECK_MSG(!canvas.ok(), "from_json: Canvas errors");
    AURORA_TEST_CHECK_MSG(canvas.error().code == "general-not-supported", "from_json: canvas error code");

    auto repeater = from_json(Json{ { "type", "Repeater" } });
    AURORA_TEST_CHECK_MSG(!repeater.ok(), "from_json: Repeater errors");
    AURORA_TEST_CHECK_MSG(repeater.error().code == "general-not-supported", "from_json: repeater error code");
}

// ---- 非法输入 ----
static void test_invalid() {
    auto empty = from_json(Json{});
    AURORA_TEST_CHECK_MSG(!empty.ok(), "from_json: empty JSON errors");
    AURORA_TEST_CHECK_MSG(empty.error().code == "io-parse-failed", "from_json: empty error code");

    auto no_type = from_json(Json::object());
    AURORA_TEST_CHECK_MSG(!no_type.ok(), "from_json: missing type errors");

    auto unknown = from_json(Json{ { "type", "Nope" } });
    AURORA_TEST_CHECK_MSG(!unknown.ok(), "from_json: unknown type errors");
    AURORA_TEST_CHECK_MSG(unknown.error().code == "widget-unknown-type", "from_json: unknown type code");
}

// ---- diff / apply_patch ----
static void test_diff_patch() {
    Json a = Json::object();
    a["x"] = 1;
    a["y"] = 2;
    Json b = a;
    b["x"] = 10;
    b["z"] = 3;

    const auto patch = diff(a, b);
    AURORA_TEST_CHECK_MSG(!patch.empty(), "diff: produces ops");
    Json applied = a;
    apply_patch(applied, patch);
    AURORA_TEST_CHECK_MSG(applied == b, "apply_patch: reproduces target (replace + add)");

    // 注：当前 diff 实现仅产出 replace / add 操作，不支持 remove（删键），
    // 故这里只验证受支持的替换+新增语义。
    // diff_into 追加到已有容器
    std::vector<JsonPatchOp> acc;
    diff_into(a, b, "", acc);
    AURORA_TEST_CHECK_MSG(!acc.empty(), "diff_into: appends ops");
}

// ---- 组件反射 ----
static void test_reflection() {
    const auto types = list_all_components();
    AURORA_TEST_CHECK_MSG(!types.empty(), "list_all_components: non-empty");
    bool has_text = false;
    for (const auto &t : types) {
        if (t == "Text") {
            has_text = true;
        }
    }
    AURORA_TEST_CHECK_MSG(has_text, "list_all_components: contains Text");

    Json schema = describe_component("Text");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component: non-empty");
    AURORA_TEST_CHECK_MSG(schema["type"] == "Text", "describe_component: type field");
    AURORA_TEST_CHECK_MSG(schema["thread"] == "main", "describe_component: thread=main");
    AURORA_TEST_CHECK_MSG(schema["is_clickable"] == false, "describe_component: Text not clickable");
    AURORA_TEST_CHECK_MSG(schema["is_container"] == false, "describe_component: Text not container");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("content"),
                          "describe_component: default_props has content");

    const auto found = search_components("butt");
    AURORA_TEST_CHECK_MSG(!found.empty(), "search_components: 'butt' matches");
    bool has_button = false;
    for (const auto &f : found) {
        if (f["type"] == "Button") {
            has_button = true;
        }
    }
    AURORA_TEST_CHECK_MSG(has_button, "search_components: found Button");
}

// ---- Phase4: Stack fit 属性序列化往返 ----
static void test_stack_props_roundtrip() {
    Stack s(std::vector<Node>{}, Alignment::TopLeft);
    s.set_fit(StackFit::Expand);
    Json j;
    s.serialize_props(j);
    AURORA_TEST_CHECK_MSG((j["fit"].get<int>()) == static_cast<int>(StackFit::Expand), "stack fit=Expand");

    Stack t(std::vector<Node>{}, Alignment::TopLeft);
    t.deserialize_props(j);
    Json k;
    t.serialize_props(k);
    AURORA_TEST_CHECK_MSG((k["fit"].get<int>()) == static_cast<int>(StackFit::Expand), "stack rt fit");
}

// ---- Phase4: RichText 注册与反射 ----
static void test_richtext_registration() {
    register_core_widgets();
    auto made = WidgetRegistry::instance().make("RichText", Json::object());
    AURORA_TEST_CHECK_MSG(made.ok(), "RichText factory registered in WidgetRegistry");

    Json schema = describe_component("RichText");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"RichText\") non-empty");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("text"),
                          "RichText schema exposes 'text' prop");

    Json node = Json::object();
    node["type"] = "RichText";
    node["props"] = Json::object();
    node["props"]["text"] = "hello rich";
    auto back = from_json(node);
    AURORA_TEST_CHECK_MSG(back.ok(), "from_json reconstructs RichText");
    if (back.ok()) {
        Json round = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(round["type"] == "RichText", "RichText round-trip preserves type");
        AURORA_TEST_CHECK_MSG(round["props"].value("text", std::string()) == "hello rich",
                              "RichText round-trip preserves text");
    }
}

// ---- Phase4: Grid 属性反射 + 序列化往返 ----
static void test_grid_roundtrip() {
    Json schema = describe_component("Grid");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Grid\") non-empty");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("columns"),
                          "Grid schema exposes 'columns'");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("gap"),
                          "Grid schema exposes 'gap'");

    const auto grid = std::make_shared<Grid>(
        std::initializer_list{ Node{ std::make_shared<Text>("a") }, Node{ std::make_shared<Text>("b") },
                               Node{ std::make_shared<Text>("c") }, Node{ std::make_shared<Text>("d") } },
        3, 8.0f);
    const Json gj = to_json(*grid);
    auto back = from_json(gj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Grid round-trips via from_json");
    if (back.ok()) {
        AURORA_TEST_CHECK_MSG(back.value()->type_name() == std::string("Grid"), "round-tripped widget is Grid");
        Json gj2 = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(gj2["props"].value("columns", 0) == 3, "Grid columns preserved");
        AURORA_TEST_CHECK_MSG(std::abs(gj2["props"].value("gap", 0.0) - 8.0) < 1e-3, "Grid gap preserved");
        AURORA_TEST_CHECK_MSG(gj2.contains("children") && gj2["children"].size() == 4, "Grid children preserved");
    }
}

// ---- Phase4: Scroll 属性反射 + 序列化往返 ----
static void test_scroll_roundtrip() {
    Json schema = describe_component("Scroll");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Scroll\") non-empty");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("step"),
                          "Scroll schema exposes 'step'");

    const auto scroll =
        std::make_shared<Scroll>(std::initializer_list{ Node{ std::make_shared<Text>("scroll content") } });
    scroll->step = 24.0f;
    const Json sj = to_json(*scroll);
    auto back = from_json(sj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Scroll round-trips via from_json");
    if (back.ok()) {
        Json sj2 = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(std::abs(sj2["props"].value("step", 0.0) - 24.0) < 1e-3, "Scroll step preserved");
    }
}

static void test_hero_roundtrip() {
    Json schema = describe_component("Hero");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Hero\") non-empty");
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("tag"),
                          "Hero schema exposes 'tag'");

    const auto hero = std::make_shared<Hero>("logo", Node{ std::make_shared<Text>("Aurora") });
    Json hj = to_json(*hero);
    AURORA_TEST_CHECK_MSG(hj["props"].value("tag", std::string{}) == "logo", "Hero tag serialized");
    auto back = from_json(hj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Hero round-trips via from_json");
    if (back.ok()) {
        auto const *hb = dynamic_cast<Hero *>(back.value().get());
        AURORA_TEST_CHECK_MSG(hb != nullptr, "from_json rebuilds Hero");
        AURORA_TEST_CHECK_MSG(hb && hb->tag() == "logo", "Hero tag preserved");
        Json hj2 = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(hj2["props"].value("tag", std::string{}) == "logo",
                              "Hero tag preserved across round-trip");
    }
}

static void run() {
    AURORA_TEST_PRINTF("=== serialization_test ===\n");
    test_to_json();
    test_round_trip();
    test_registry();
    test_non_restorable();
    test_invalid();
    test_diff_patch();
    test_reflection();
    test_stack_props_roundtrip();
    test_richtext_registration();
    test_grid_roundtrip();
    test_scroll_roundtrip();
    test_hero_roundtrip();
}
} // namespace aurora::tests::sec_serialization

namespace aurora::tests::sec_codegen {

// 构造 { type, props, children } 结构快照（与 to_json 输出同构）。
static auto make_button(const std::string &label) -> Json {
    Json btn = Json::object();
    btn["type"] = "Button";
    btn["props"] = Json::object();
    btn["props"]["label"] = label;
    return btn;
}

static void test_codegen_fluent() {
    Json node = Json::object();
    node["type"] = "Column";
    node["children"] = Json::array();
    node["children"].push_back(make_button("OK"));
    node["children"].push_back(make_button("Cancel"));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Column{") != std::string::npos, "codegen: fluent emits au::Column{");
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: fluent emits au::Button(...)");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"OK\"") != std::string::npos, "codegen: fluent emits .label = \"OK\"");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"Cancel\"") != std::string::npos,
                          "codegen: fluent emits .label = \"Cancel\"");
}

static void test_codegen_grid_fluent() {
    // 多列 Grid 须在子项后追加列数。
    Json node = Json::object();
    node["type"] = "Grid";
    node["props"] = Json::object();
    node["props"]["columns"] = 2;
    node["children"] = Json::array();
    node["children"].push_back(make_button("A"));
    node["children"].push_back(make_button("B"));

    const std::string code = to_code(node);
    AURORA_TEST_CHECK_MSG(code.find("au::Grid{") != std::string::npos, "codegen: grid fluent emits au::Grid{");
    // 列数 2 作为独立行追加于子项之后（缩进 + "2" + 收尾 }）。
    AURORA_TEST_CHECK_MSG(code.find("au::Button(au::ButtonProps{") != std::string::npos,
                          "codegen: grid fluent emits au::Button(...)");
    AURORA_TEST_CHECK_MSG(code.find("2\n}") != std::string::npos || code.find("2}") != std::string::npos,
                          "codegen: grid fluent appends column count");
}

static void test_codegen_styles() {
    Json node = Json::object();
    node["type"] = "Column";
    node["children"] = Json::array();
    node["children"].push_back(make_button("OK"));

    const std::string di = to_code(node, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("au::Column{") != std::string::npos, "codegen: DesignatedInit emits au::Column{");
    AURORA_TEST_CHECK_MSG(di.find("au::Button{") != std::string::npos, "codegen: DesignatedInit emits au::Button{");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("auto __w0 = au::Column{};") != std::string::npos,
                          "codegen: StepByStep declares Column var");
    AURORA_TEST_CHECK_MSG(sb.find("auto __w1 = au::Button{};") != std::string::npos,
                          "codegen: StepByStep declares Button var");
    AURORA_TEST_CHECK_MSG(sb.find("__w1.label = \"OK\";") != std::string::npos, "codegen: StepByStep assigns label");
    // 注意：StepByStep 把子项声明内嵌进父项的 { } 列表，故断言「打开 children 列表」且「引用子变量 __w1」。
    AURORA_TEST_CHECK_MSG(sb.find("__w0.children = {") != std::string::npos, "codegen: StepByStep opens children list");
    AURORA_TEST_CHECK_MSG(sb.find("__w1") != std::string::npos, "codegen: StepByStep references child var __w1");

    // 默认风格等价于 Fluent。
    AURORA_TEST_CHECK_MSG(to_code(node) == to_code(node, CodeStyle::Fluent), "codegen: default style == Fluent");
}

static void run() {
    AURORA_TEST_PRINTF("=== test_codegen ===\n");
    test_codegen_fluent();
    test_codegen_grid_fluent();
    test_codegen_styles();
}
} // namespace aurora::tests::sec_codegen

namespace aurora::tests::sec_codegen_extended {

// ---------- 辅助：构造含单个 prop 的 widget 节点 ----------
static auto make_node_with_props(const std::string &type, const Json &props) -> Json {
    Json node = Json::object();
    node["type"] = type;
    node["props"] = props;
    node["children"] = Json::array();
    return node;
}

// ========== 各类型测试 ==========

static void test_string_props() {
    // content / label 等普通字符串
    Json props = Json::object();
    props["content"] = "Hello";
    props["label"] = "OK";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".content = \"Hello\"") != std::string::npos, "emit_props: string content");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"OK\"") != std::string::npos, "emit_props: string label");
}

static void test_bool_prop() {
    Json props = Json::object();
    props["show"] = true;
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".show = true") != std::string::npos, "emit_props: bool true");

    props["show"] = false;
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".show = false") != std::string::npos, "emit_props: bool false");
}

static void test_float_prop() {
    Json props = Json::object();
    props["font_size"] = 14.0;
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".font_size = ") != std::string::npos, "emit_props: float prop present");
    AURORA_TEST_CHECK_MSG(code.find('f') != std::string::npos, "emit_props: float has f suffix");
}

static void test_int_prop() {
    Json props = Json::object();
    props["flex"] = 1;
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find(".flex = 1") != std::string::npos, "emit_props: int prop");
}

static void test_length_array_px() {
    // 实际序列化格式: ["px", 100]
    Json props = Json::object();
    props["width"] = Json::array({ "px", 100.0f });
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "emit_props: Length px array");
}

static void test_length_array_percent() {
    Json props = Json::object();
    props["width"] = Json::array({ "percent", 0.5f });
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::percent(") != std::string::npos, "emit_props: Length percent array");
}

static void test_length_legacy_object() {
    // 旧格式兼容: {"value": 100, "unit": "px"}
    Json props = Json::object();
    props["width"] = { { "value", 100.0f }, { "unit", "px" } };
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "emit_props: Length legacy object px");
}

static void test_length_legacy_object_pct() {
    Json props = Json::object();
    props["width"] = { { "value", 50.0f }, { "unit", "pct" } };
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::percent(") != std::string::npos, "emit_props: Length legacy object pct");
}

static void test_length_auto_fill() {
    Json props = Json::object();
    props["width"] = "auto";
    props["height"] = "fill";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::auto_length()") != std::string::npos, "emit_props: Length auto string");
    AURORA_TEST_CHECK_MSG(code.find("au::fill()") != std::string::npos, "emit_props: Length fill string");
}

static void test_color_array() {
    // Color 序列化格式: [r,g,b,a]
    Json props = Json::object();
    props["color"] = Json::array({ 255, 0, 0, 255 });
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("Color{") != std::string::npos, "emit_props: Color array has Color{");
    AURORA_TEST_CHECK_MSG(code.find("255,0,0,255") != std::string::npos, "emit_props: Color RGBA values");
}

static void test_edge_insets_object() {
    Json props = Json::object();
    props["padding"] = { { "top", 8.0f }, { "right", 12.0f }, { "bottom", 8.0f }, { "left", 12.0f } };
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("EdgeInsets{") != std::string::npos, "emit_props: EdgeInsets object");
}

static void test_enum_text_align() {
    Json props = Json::object();
    props["text_align"] = "Center";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextAlign::Center") != std::string::npos, "emit_props: enum TextAlign");
}

static void test_enum_text_overflow() {
    Json props = Json::object();
    props["text_overflow"] = "Ellipsis";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextOverflow::Ellipsis") != std::string::npos, "emit_props: enum TextOverflow");
}

static void test_enum_main_axis_alignment() {
    Json props = Json::object();
    props["main_axis_alignment"] = "SpaceBetween";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("MainAxisAlignment::SpaceBetween") != std::string::npos,
                          "emit_props: enum MainAxisAlignment");
}

static void test_enum_cross_axis_alignment() {
    Json props = Json::object();
    props["cross_axis_alignment"] = "Stretch";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("CrossAxisAlignment::Stretch") != std::string::npos,
                          "emit_props: enum CrossAxisAlignment");
}

static void test_enum_main_axis_size() {
    Json props = Json::object();
    props["main_axis_size"] = "Max";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("MainAxisSize::Max") != std::string::npos, "emit_props: enum MainAxisSize");
}

static void test_enum_stack_fit() {
    Json props = Json::object();
    props["stack_fit"] = "Expand";
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("StackFit::Expand") != std::string::npos, "emit_props: enum StackFit");
}

static void test_enum_box_fit() {
    Json props = Json::object();
    props["box_fit"] = "Cover";
    const std::string code = to_code(make_node_with_props("Image", props));
    AURORA_TEST_CHECK_MSG(code.find("BoxFit::Cover") != std::string::npos, "emit_props: enum BoxFit");
}

static void test_enum_overflow_strategy() {
    Json props = Json::object();
    props["overflow_strategy"] = "Scroll";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("OverflowStrategy::Scroll") != std::string::npos,
                          "emit_props: enum OverflowStrategy");
}

// ---- 真实序列化属性名（to_json 的产出键名），非合成名 ----

static void test_enum_real_key_alignment() {
    Json props = Json::object();
    props["alignment"] = "TopLeft"; // Stack.alignment 的真实键名
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("Alignment::TopLeft") != std::string::npos,
                          "emit_props: real key alignment → Alignment");
}

static void test_enum_real_key_overflow() {
    Json props = Json::object();
    props["overflow"] = "Ellipsis"; // Text.overflow 的真实键名
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextOverflow::Ellipsis") != std::string::npos,
                          "emit_props: real key overflow → TextOverflow");
}

static void test_enum_real_key_side() {
    Json props = Json::object();
    props["side"] = "Right"; // Drawer.side 的真实键名
    const std::string code = to_code(make_node_with_props("Drawer", props));
    AURORA_TEST_CHECK_MSG(code.find("DrawerSide::Right") != std::string::npos,
                          "emit_props: real key side → DrawerSide");
}

static void test_enum_real_key_position() {
    Json props = Json::object();
    props["position"] = "Top"; // ToastHost.position 的真实键名
    const std::string code = to_code(make_node_with_props("ToastHost", props));
    AURORA_TEST_CHECK_MSG(code.find("ToastPosition::Top") != std::string::npos,
                          "emit_props: real key position → ToastPosition");
}

static void test_enum_ambiguous_keys_not_guessed() {
    // fit / orientation 无法按键消歧（StackFit vs BoxFit、Orientation vs SplitterOrientation），
    // 因此刻意保持字符串输出，等属性声明类型透传后再升级——测试锁住这一现状。
    Json props = Json::object();
    props["fit"] = "Cover";
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("\"Cover\"") != std::string::npos, "emit_props: ambiguous key fit stays as string");
}

static void test_font_weight() {
    Json props = Json::object();
    props["font_weight"] = "700";
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Bold") != std::string::npos, "emit_props: FontWeight 700 → Bold");

    props["font_weight"] = "400";
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Normal") != std::string::npos, "emit_props: FontWeight 400 → Normal");
}

static void test_font_style() {
    Json props = Json::object();
    props["font_style"] = "Italic";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontStyle::Italic") != std::string::npos, "emit_props: enum FontStyle");
}

static void test_text_decoration_array() {
    Json props = Json::object();
    props["text_decoration"] = Json::array({ "Underline" });
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::Underline") != std::string::npos,
                          "emit_props: TextDecoration array Underline");

    props["text_decoration"] = Json::array({ "Underline", "LineThrough" });
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::Underline") != std::string::npos &&
                              code.find("TextDecoration::LineThrough") != std::string::npos &&
                              code.find(" | ") != std::string::npos,
                          "emit_props: TextDecoration combined with |");

    props["text_decoration"] = Json::array({ "None" });
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::None") != std::string::npos, "emit_props: TextDecoration None");
}

// ========== 三种 CodeStyle 测试 ==========

static void test_all_styles_color() {
    Json props = Json::object();
    props["color"] = Json::array({ 0, 128, 255, 255 });

    Json node = Json::object();
    node["type"] = "Text";
    node["props"] = props;
    node["children"] = Json::array();

    // Fluent (default)
    const std::string fluent = to_code(node, CodeStyle::Fluent);
    AURORA_TEST_CHECK_MSG(fluent.find("Color{") != std::string::npos, "styles: Fluent has Color{}");

    // DesignatedInit
    const std::string di = to_code(node, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("Color{") != std::string::npos, "styles: DesignatedInit has Color{}");

    // StepByStep
    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("Color{") != std::string::npos, "styles: StepByStep has Color{}");
    AURORA_TEST_CHECK_MSG(sb.find("__w0.color = ") != std::string::npos, "styles: StepByStep assigns color via __w0");
}

static void test_all_styles_enum() {
    Json props = Json::object();
    props["text_align"] = "Right";

    Json node = Json::object();
    node["type"] = "Text";
    node["props"] = props;
    node["children"] = Json::array();

    const std::string fluent = to_code(node, CodeStyle::Fluent);
    AURORA_TEST_CHECK_MSG(fluent.find("TextAlign::Right") != std::string::npos, "styles: Fluent enum");

    const std::string di = to_code(node, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK_MSG(di.find("TextAlign::Right") != std::string::npos, "styles: DesignatedInit enum");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("TextAlign::Right") != std::string::npos, "styles: StepByStep enum");
}

static void test_all_styles_edge_insets() {
    Json props = Json::object();
    props["padding"] = { { "top", 4.0f }, { "right", 8.0f }, { "bottom", 4.0f }, { "left", 8.0f } };

    Json node = Json::object();
    node["type"] = "Column";
    node["props"] = props;
    node["children"] = Json::array();

    const std::string fluent = to_code(node, CodeStyle::Fluent);
    AURORA_TEST_CHECK_MSG(fluent.find("EdgeInsets{") != std::string::npos, "styles: Fluent EdgeInsets");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("EdgeInsets{") != std::string::npos, "styles: StepByStep EdgeInsets");
}

// ========== 综合：多属性混合 ==========

static void test_multi_prop_mixed() {
    Json props = Json::object();
    props["content"] = "Hello";
    props["font_size"] = 16.0f;
    props["show"] = true;
    props["text_align"] = "Center";
    props["font_weight"] = "700";
    props["color"] = Json::array({ 255, 0, 0, 255 });
    props["width"] = Json::array({ "px", 200.0f });

    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".content = \"Hello\"") != std::string::npos, "mixed: content");
    AURORA_TEST_CHECK_MSG(code.find(".show = true") != std::string::npos, "mixed: show");
    AURORA_TEST_CHECK_MSG(code.find("TextAlign::Center") != std::string::npos, "mixed: text_align");
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Bold") != std::string::npos, "mixed: font_weight");
    AURORA_TEST_CHECK_MSG(code.find("Color{") != std::string::npos, "mixed: color");
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "mixed: width px");
}

static void run() {
    AURORA_TEST_PRINTF("=== test_codegen_extended ===\n");

    // 基础类型
    test_string_props();
    test_bool_prop();
    test_float_prop();
    test_int_prop();

    // Length
    test_length_array_px();
    test_length_array_percent();
    test_length_legacy_object();
    test_length_legacy_object_pct();
    test_length_auto_fill();

    // Color / EdgeInsets
    test_color_array();
    test_edge_insets_object();

    // 枚举
    test_enum_text_align();
    test_enum_text_overflow();
    test_enum_main_axis_alignment();
    test_enum_cross_axis_alignment();
    test_enum_main_axis_size();
    test_enum_stack_fit();
    test_enum_box_fit();
    test_enum_overflow_strategy();
    // 真实序列化键名（to_json 产出的属性名）
    test_enum_real_key_alignment();
    test_enum_real_key_overflow();
    test_enum_real_key_side();
    test_enum_real_key_position();
    test_enum_ambiguous_keys_not_guessed();
    test_font_weight();
    test_font_style();
    test_text_decoration_array();

    // 三种风格
    test_all_styles_color();
    test_all_styles_enum();
    test_all_styles_edge_insets();

    // 混合
    test_multi_prop_mixed();
}
} // namespace aurora::tests::sec_codegen_extended

namespace aurora::tests::sec_to_yaml {

// ---- 基本标量类型 ----
static void test_scalar_types() {
    // null
    AURORA_TEST_CHECK_EQ(to_yaml(Json(nullptr)), std::string("null"));

    // bool
    AURORA_TEST_CHECK_EQ(to_yaml(Json(true)), std::string("true"));
    AURORA_TEST_CHECK_EQ(to_yaml(Json(false)), std::string("false"));

    // integer
    AURORA_TEST_CHECK_EQ(to_yaml(Json(42)), std::string("42"));
    AURORA_TEST_CHECK_EQ(to_yaml(Json(-7)), std::string("-7"));
    AURORA_TEST_CHECK_EQ(to_yaml(Json(0)), std::string("0"));

    // float
    {
        const std::string s = to_yaml(Json(3.14));
        AURORA_TEST_CHECK_MSG(s.find("3.14") != std::string::npos, "float 3.14 output contains 3.14");
    }
}

// ---- 字符串引号逻辑 ----
static void test_string_quoting() {
    // 纯字母字符串不需要引号
    {
        const std::string s = to_yaml(Json(std::string("hello")));
        AURORA_TEST_CHECK_EQ(s, std::string("hello"));
    }

    // 空字符串必须加引号
    {
        const std::string s = to_yaml(Json(std::string("")));
        AURORA_TEST_CHECK_EQ(s, std::string("\"\""));
    }

    // 数值字符串（如 FontWeight "700"）必须加引号
    {
        const std::string s = to_yaml(Json(std::string("700")));
        AURORA_TEST_CHECK_MSG(s == "\"700\"", "FontWeight '700' must be quoted");
    }

    // 布尔字面量字符串必须加引号
    {
        const std::string s = to_yaml(Json(std::string("true")));
        AURORA_TEST_CHECK_EQ(s, std::string("\"true\""));
    }
    {
        const std::string s = to_yaml(Json(std::string("false")));
        AURORA_TEST_CHECK_EQ(s, std::string("\"false\""));
    }

    // null 字面量字符串必须加引号
    {
        const std::string s = to_yaml(Json(std::string("null")));
        AURORA_TEST_CHECK_EQ(s, std::string("\"null\""));
    }

    // 含特殊字符的字符串必须加引号
    {
        const std::string s = to_yaml(Json(std::string("key: value")));
        AURORA_TEST_CHECK_MSG(s.find('"') != std::string::npos, "string with colon must be quoted");
    }
    {
        const std::string s = to_yaml(Json(std::string("#comment")));
        AURORA_TEST_CHECK_MSG(s.find('"') != std::string::npos, "string with # must be quoted");
    }

    // 含引号转义的字符串
    {
        const std::string s = to_yaml(Json(std::string("say \"hi\"")));
        AURORA_TEST_CHECK_MSG(s.find("\\\"") != std::string::npos, "embedded quotes are escaped");
    }
}

// ---- 对象输出 ----
static void test_object() {
    Json j = Json::object();
    j["name"] = "Aurora";
    j["version"] = 1;

    const std::string s = to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("name: Aurora") != std::string::npos, "object key: unquoted string value");
    AURORA_TEST_CHECK_MSG(s.find("version: 1") != std::string::npos, "object key: integer value");
}

// ---- 数组输出 ----
static void test_array() {
    const Json j = Json::array({ 1, 2, 3 });
    const std::string s = to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("- 1") != std::string::npos, "array contains - 1");
    AURORA_TEST_CHECK_MSG(s.find("- 2") != std::string::npos, "array contains - 2");
    AURORA_TEST_CHECK_MSG(s.find("- 3") != std::string::npos, "array contains - 3");
}

// ---- 空容器 ----
static void test_empty_containers() {
    AURORA_TEST_CHECK_EQ(to_yaml(Json::object()), std::string("{}"));
    AURORA_TEST_CHECK_EQ(to_yaml(Json::array()), std::string("[]"));
}

// ---- 嵌套结构 ----
static void test_nested() {
    Json j = Json::object();
    j["title"] = "Test";
    Json inner = Json::object();
    inner["x"] = 10;
    inner["y"] = 20;
    j["pos"] = inner;

    const std::string s = to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("title: Test") != std::string::npos, "nested: top-level key");
    AURORA_TEST_CHECK_MSG(s.find("pos:") != std::string::npos, "nested: inner object key");
    AURORA_TEST_CHECK_MSG(s.find("x: 10") != std::string::npos, "nested: inner key x");
    AURORA_TEST_CHECK_MSG(s.find("y: 20") != std::string::npos, "nested: inner key y");
}

// ---- 完整 widget 树 YAML 输出 ----
static void test_widget_yaml() {
    Node root = Column{ Text{ "Hello" }, Button{ "OK" } };
    const Json j = to_json(root.widget());
    const std::string yaml = to_yaml(j);

    AURORA_TEST_CHECK_MSG(yaml.find("type: Column") != std::string::npos, "widget yaml: Column type");
    AURORA_TEST_CHECK_MSG(yaml.find("type: Text") != std::string::npos, "widget yaml: Text type");
    AURORA_TEST_CHECK_MSG(yaml.find("content: Hello") != std::string::npos, "widget yaml: Text content");
    AURORA_TEST_CHECK_MSG(yaml.find("type: Button") != std::string::npos, "widget yaml: Button type");
}

// ---- 浮点特殊值 ----
static void test_float_special() {
    // 整数浮点应带小数点
    {
        const std::string s = to_yaml(Json(1.0));
        AURORA_TEST_CHECK_MSG(s.find('.') != std::string::npos, "float 1.0 has decimal point");
    }
}

static void run() {
    AURORA_TEST_PRINTF("=== test_to_yaml ===\n");
    test_scalar_types();
    test_string_quoting();
    test_object();
    test_array();
    test_empty_containers();
    test_nested();
    test_widget_yaml();
    test_float_special();
}
} // namespace aurora::tests::sec_to_yaml

AURORA_TEST() {
    aurora::tests::sec_serialization::run();
    aurora::tests::sec_codegen::run();
    aurora::tests::sec_codegen_extended::run();
    aurora::tests::sec_to_yaml::run();
}
