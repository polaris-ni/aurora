/// 测试类型: unit
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: utest_serialization 单元测试
///
// 目标源单元（历史映射，保留供审计）: Serialization + Codegen + Yaml + Serialization
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// Codegen(to_code 三风格代码生成)、Yaml(to_yaml 发射器)。

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/yaml.h"
#include "aurora_test_harness.h"

using au::Alignment;
using au::Button;
using au::Column;
using au::describe_component;
using au::Grid;
using au::Hero;
using au::Json;
using au::list_all_components;
using au::Node;
using au::Row;
using au::Scroll;
using au::search_components;
using au::Stack;
using au::StackFit;
using au::Text;
using au::serialization::CodeStyle;
using au::serialization::diff;
using au::serialization::from_json;
using au::serialization::JsonPatchOp;
using au::serialization::register_core_widgets;
using au::serialization::to_code;
using au::serialization::to_json;
using au::serialization::to_yaml;
using au::serialization::WidgetRegistry;

namespace aurora::test_cases::utest_serialization {

namespace aurora::tests::sec_serialization {

// ---- to_json 结构 ----
static void test_to_json() {
    Node root = Column{Text{"Hello"}, Text{"World"}};
    Json j = to_json(root.widget());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["type"] == "Column", "to_json: root type Column");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j.contains("children") && j["children"].size() == 2, "to_json: two children");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["children"][0]["type"] == "Text", "to_json: child[0] is Text");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["children"][0]["props"]["content"] == "Hello", "to_json: child content serialized");

    const Text t{"Hi"};
    Json jt = to_json(t);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(jt["type"] == "Text", "to_json: Text type");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(jt["props"]["content"] == "Hi", "to_json: Text content prop");
}

// ---- from_json 往返 ----
static void test_round_trip() {
    Node root = Column{Text{"Hello"}, Row{Button{"Click"}, Text{"World"}}};
    Json j = to_json(root.widget());

    auto back = from_json(j);
    AURORA_TEST_CHECK_MSG(back.ok(), "from_json: rebuilds tree");
    if (back.ok()) {
        Json j2 = to_json(*back.value());
        AURORA_TEST_CHECK_MSG(j == j2, "serialization: round-trip produces equal JSON");
    }

    // 单叶控件往返
    Text t{"Hi"};
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
    auto canvas = from_json(Json{{"type", "Canvas"}});
    AURORA_TEST_CHECK_MSG(!canvas.ok(), "from_json: Canvas errors");
    AURORA_TEST_CHECK_MSG(canvas.error().code == "general-not-supported", "from_json: canvas error code");

    auto repeater = from_json(Json{{"type", "Repeater"}});
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

    auto unknown = from_json(Json{{"type", "Nope"}});
    AURORA_TEST_CHECK_MSG(!unknown.ok(), "from_json: unknown type errors");
    AURORA_TEST_CHECK_MSG(unknown.error().code == "widget-unknown-type", "from_json: unknown type code");
}

// ---- diff / apply_patch ----
static void test_diff_patch() {
    Json a = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    a["x"] = 1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    a["y"] = 2;
    Json b = a;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    b["x"] = 10;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema["type"] == "Text", "describe_component: type field");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema["thread"] == "main", "describe_component: thread=main");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema["is_clickable"] == false, "describe_component: Text not clickable");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema["is_container"] == false, "describe_component: Text not container");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("content"),
                          "describe_component: default_props has content");

    const auto found = search_components("butt");
    AURORA_TEST_CHECK_MSG(!found.empty(), "search_components: 'butt' matches");
    bool has_button = false;
    for (const auto &f : found) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG((j["fit"].get<int>()) == static_cast<int>(StackFit::Expand), "stack fit=Expand");

    Stack t(std::vector<Node>{}, Alignment::TopLeft);
    t.deserialize_props(j);
    Json k;
    t.serialize_props(k);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG((k["fit"].get<int>()) == static_cast<int>(StackFit::Expand), "stack rt fit");
}

// ---- Phase4: RichText 注册与反射 ----
static void test_richtext_registration() {
    register_core_widgets();
    auto made = WidgetRegistry::instance().make("RichText", Json::object());
    AURORA_TEST_CHECK_MSG(made.ok(), "RichText factory registered in WidgetRegistry");

    Json schema = describe_component("RichText");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"RichText\") non-empty");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("text"),
                          "RichText schema exposes 'text' prop");

    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "RichText";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"]["text"] = "hello rich";
    auto back = from_json(node);
    AURORA_TEST_CHECK_MSG(back.ok(), "from_json reconstructs RichText");
    if (back.ok()) {
        Json round = to_json(*back.value());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(round["type"] == "RichText", "RichText round-trip preserves type");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(round["props"].value("text", std::string()) == "hello rich",
                              "RichText round-trip preserves text");
    }
}

// ---- Phase4: Grid 属性反射 + 序列化往返 ----
static void test_grid_roundtrip() {
    Json schema = describe_component("Grid");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Grid\") non-empty");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("columns"),
                          "Grid schema exposes 'columns'");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("gap"),
                          "Grid schema exposes 'gap'");

    const auto grid = std::make_shared<Grid>(
        std::initializer_list{Node{std::make_shared<Text>("a")}, Node{std::make_shared<Text>("b")},
                              Node{std::make_shared<Text>("c")}, Node{std::make_shared<Text>("d")}},
        3, 8.0F);
    const Json gj = to_json(*grid);
    auto back = from_json(gj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Grid round-trips via from_json");
    if (back.ok()) {
        AURORA_TEST_CHECK_MSG(back.value()->type_name() == std::string("Grid"), "round-tripped widget is Grid");
        Json gj2 = to_json(*back.value());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(gj2["props"].value("columns", 0) == 3, "Grid columns preserved");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::abs(gj2["props"].value("gap", 0.0) - 8.0) < 1e-3, "Grid gap preserved");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(gj2.contains("children") && gj2["children"].size() == 4, "Grid children preserved");
    }
}

// ---- Phase4: Scroll 属性反射 + 序列化往返 ----
static void test_scroll_roundtrip() {
    Json schema = describe_component("Scroll");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Scroll\") non-empty");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("step"),
                          "Scroll schema exposes 'step'");

    const auto scroll = std::make_shared<Scroll>(std::initializer_list{Node{std::make_shared<Text>("scroll content")}});
    scroll->step = 24.0F;
    const Json sj = to_json(*scroll);
    auto back = from_json(sj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Scroll round-trips via from_json");
    if (back.ok()) {
        Json sj2 = to_json(*back.value());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::abs(sj2["props"].value("step", 0.0) - 24.0) < 1e-3, "Scroll step preserved");
    }
}

static void test_hero_roundtrip() {
    Json schema = describe_component("Hero");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(\"Hero\") non-empty");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(schema.contains("default_props") && schema["default_props"].contains("tag"),
                          "Hero schema exposes 'tag'");

    const auto hero = std::make_shared<Hero>("logo", Node{std::make_shared<Text>("Aurora")});
    Json hj = to_json(*hero);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(hj["props"].value("tag", std::string{}) == "logo", "Hero tag serialized");
    auto back = from_json(hj);
    AURORA_TEST_CHECK_MSG(back.ok(), "Hero round-trips via from_json");
    if (back.ok()) {
        auto const *hb = dynamic_cast<Hero *>(back.value().get());
        AURORA_TEST_CHECK_MSG(hb != nullptr, "from_json rebuilds Hero");
        AURORA_TEST_CHECK_MSG(hb && hb->tag() == "logo", "Hero tag preserved");
        Json hj2 = to_json(*back.value());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
}  // namespace aurora::tests::sec_serialization

AURORA_TEST() {
    aurora::tests::sec_serialization::run();
}

}  // namespace aurora::test_cases::utest_serialization
