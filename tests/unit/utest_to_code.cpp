// 目标源单元：widget/serialization.h + widget/codegen.h + widget/yaml.h + src/aurora/widget/serialization.cpp
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// widget/codegen.h(to_code 三风格代码生成)、widget/yaml.h(to_yaml 发射器)。

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

// （自 utest_serialization.cpp 拆分：to_code 基础 + 扩展/风格/多属性段）

namespace aurora::test_cases::utest_to_code {

namespace aurora::tests::sec_codegen {

// 构造 { type, props, children } 结构快照（与 to_json 输出同构）。
static auto make_button(const std::string &label) -> Json {
    Json btn = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    btn["type"] = "Button";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    btn["props"] = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    btn["props"]["label"] = label;
    return btn;
}

static void test_codegen_fluent() {
    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Column";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"] = Json::array();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"].push_back(make_button("OK"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Grid";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"]["columns"] = 2;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"] = Json::array();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"].push_back(make_button("A"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Column";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"] = Json::array();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
}  // namespace aurora::tests::sec_codegen

namespace aurora::tests::sec_codegen_extended {

// ---------- 辅助：构造含单个 prop 的 widget 节点 ----------
static auto make_node_with_props(const std::string &type, const Json &props) -> Json {
    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = type;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = props;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"] = Json::array();
    return node;
}

// ========== 各类型测试 ==========

static void test_string_props() {
    // content / label 等普通字符串
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["content"] = "Hello";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["label"] = "OK";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".content = \"Hello\"") != std::string::npos, "emit_props: string content");
    AURORA_TEST_CHECK_MSG(code.find(".label = \"OK\"") != std::string::npos, "emit_props: string label");
}

static void test_bool_prop() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["show"] = true;
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".show = true") != std::string::npos, "emit_props: bool true");

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["show"] = false;
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".show = false") != std::string::npos, "emit_props: bool false");
}

static void test_float_prop() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_size"] = 14.0;
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find(".font_size = ") != std::string::npos, "emit_props: float prop present");
    AURORA_TEST_CHECK_MSG(code.find('f') != std::string::npos, "emit_props: float has f suffix");
}

static void test_int_prop() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["flex"] = 1;
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find(".flex = 1") != std::string::npos, "emit_props: int prop");
}

static void test_length_array_px() {
    // 实际序列化格式: ["px", 100]
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = Json::array({"px", 100.0F});
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "emit_props: Length px array");
}

static void test_length_array_percent() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = Json::array({"percent", 0.5F});
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::percent(") != std::string::npos, "emit_props: Length percent array");
}

static void test_length_legacy_object() {
    // 旧格式兼容: {"value": 100, "unit": "px"}
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = {{"value", 100.0F}, {"unit", "px"}};
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::px(") != std::string::npos, "emit_props: Length legacy object px");
}

static void test_length_legacy_object_pct() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = {{"value", 50.0F}, {"unit", "pct"}};
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::percent(") != std::string::npos, "emit_props: Length legacy object pct");
}

static void test_length_auto_fill() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = "auto";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["height"] = "fill";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("au::auto_length()") != std::string::npos, "emit_props: Length auto string");
    AURORA_TEST_CHECK_MSG(code.find("au::fill()") != std::string::npos, "emit_props: Length fill string");
}

static void test_color_array() {
    // Color 序列化格式: [r,g,b,a]
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["color"] = Json::array({255, 0, 0, 255});
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("Color{") != std::string::npos, "emit_props: Color array has Color{");
    AURORA_TEST_CHECK_MSG(code.find("255,0,0,255") != std::string::npos, "emit_props: Color RGBA values");
}

static void test_edge_insets_object() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["padding"] = {{"top", 8.0F}, {"right", 12.0F}, {"bottom", 8.0F}, {"left", 12.0F}};
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("EdgeInsets{") != std::string::npos, "emit_props: EdgeInsets object");
}

static void test_enum_text_align() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_align"] = "Center";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextAlign::Center") != std::string::npos, "emit_props: enum TextAlign");
}

static void test_enum_text_overflow() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_overflow"] = "Ellipsis";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextOverflow::Ellipsis") != std::string::npos, "emit_props: enum TextOverflow");
}

static void test_enum_main_axis_alignment() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["main_axis_alignment"] = "SpaceBetween";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("MainAxisAlignment::SpaceBetween") != std::string::npos,
                          "emit_props: enum MainAxisAlignment");
}

static void test_enum_cross_axis_alignment() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["cross_axis_alignment"] = "Stretch";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("CrossAxisAlignment::Stretch") != std::string::npos,
                          "emit_props: enum CrossAxisAlignment");
}

static void test_enum_main_axis_size() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["main_axis_size"] = "Max";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("MainAxisSize::Max") != std::string::npos, "emit_props: enum MainAxisSize");
}

static void test_enum_stack_fit() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["stack_fit"] = "Expand";
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("StackFit::Expand") != std::string::npos, "emit_props: enum StackFit");
}

static void test_enum_box_fit() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["box_fit"] = "Cover";
    const std::string code = to_code(make_node_with_props("Image", props));
    AURORA_TEST_CHECK_MSG(code.find("BoxFit::Cover") != std::string::npos, "emit_props: enum BoxFit");
}

static void test_enum_overflow_strategy() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["overflow_strategy"] = "Scroll";
    const std::string code = to_code(make_node_with_props("Column", props));
    AURORA_TEST_CHECK_MSG(code.find("OverflowStrategy::Scroll") != std::string::npos,
                          "emit_props: enum OverflowStrategy");
}

// ---- 真实序列化属性名（to_json 的产出键名），非合成名 ----

static void test_enum_real_key_alignment() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["alignment"] = "TopLeft";  // Stack.alignment 的真实键名
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("Alignment::TopLeft") != std::string::npos,
                          "emit_props: real key alignment → Alignment");
}

static void test_enum_real_key_overflow() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["overflow"] = "Ellipsis";  // Text.overflow 的真实键名
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextOverflow::Ellipsis") != std::string::npos,
                          "emit_props: real key overflow → TextOverflow");
}

static void test_enum_real_key_side() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["side"] = "Right";  // Drawer.side 的真实键名
    const std::string code = to_code(make_node_with_props("Drawer", props));
    AURORA_TEST_CHECK_MSG(code.find("DrawerSide::Right") != std::string::npos,
                          "emit_props: real key side → DrawerSide");
}

static void test_enum_real_key_position() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["position"] = "Top";  // ToastHost.position 的真实键名
    const std::string code = to_code(make_node_with_props("ToastHost", props));
    AURORA_TEST_CHECK_MSG(code.find("ToastPosition::Top") != std::string::npos,
                          "emit_props: real key position → ToastPosition");
}

static void test_enum_ambiguous_keys_not_guessed() {
    // fit / orientation 无法按键消歧（StackFit vs BoxFit、Orientation vs SplitterOrientation），
    // 因此刻意保持字符串输出，等属性声明类型透传后再升级——测试锁住这一现状。
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["fit"] = "Cover";
    const std::string code = to_code(make_node_with_props("Stack", props));
    AURORA_TEST_CHECK_MSG(code.find("\"Cover\"") != std::string::npos, "emit_props: ambiguous key fit stays as string");
}

static void test_font_weight() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_weight"] = "700";
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Bold") != std::string::npos, "emit_props: FontWeight 700 → Bold");

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_weight"] = "400";
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontWeight::Normal") != std::string::npos, "emit_props: FontWeight 400 → Normal");
}

static void test_font_style() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_style"] = "Italic";
    const std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("FontStyle::Italic") != std::string::npos, "emit_props: enum FontStyle");
}

static void test_text_decoration_array() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_decoration"] = Json::array({"Underline"});
    std::string code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::Underline") != std::string::npos,
                          "emit_props: TextDecoration array Underline");

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_decoration"] = Json::array({"Underline", "LineThrough"});
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::Underline") != std::string::npos &&
                              code.find("TextDecoration::LineThrough") != std::string::npos &&
                              code.find(" | ") != std::string::npos,
                          "emit_props: TextDecoration combined with |");

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_decoration"] = Json::array({"None"});
    code = to_code(make_node_with_props("Text", props));
    AURORA_TEST_CHECK_MSG(code.find("TextDecoration::None") != std::string::npos, "emit_props: TextDecoration None");
}

// ========== 三种 CodeStyle 测试 ==========

static void test_all_styles_color() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["color"] = Json::array({0, 128, 255, 255});

    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Text";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = props;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_align"] = "Right";

    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Text";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = props;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["padding"] = {{"top", 4.0F}, {"right", 8.0F}, {"bottom", 4.0F}, {"left", 8.0F}};

    Json node = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["type"] = "Column";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["props"] = props;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    node["children"] = Json::array();

    const std::string fluent = to_code(node, CodeStyle::Fluent);
    AURORA_TEST_CHECK_MSG(fluent.find("EdgeInsets{") != std::string::npos, "styles: Fluent EdgeInsets");

    const std::string sb = to_code(node, CodeStyle::StepByStep);
    AURORA_TEST_CHECK_MSG(sb.find("EdgeInsets{") != std::string::npos, "styles: StepByStep EdgeInsets");
}

// ========== 综合：多属性混合 ==========

static void test_multi_prop_mixed() {
    Json props = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["content"] = "Hello";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_size"] = 16.0F;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["show"] = true;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text_align"] = "Center";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["font_weight"] = "700";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["color"] = Json::array({255, 0, 0, 255});
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["width"] = Json::array({"px", 200.0F});

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
}  // namespace aurora::tests::sec_codegen_extended

AURORA_TEST() {
    aurora::tests::sec_codegen::run();
    aurora::tests::sec_codegen_extended::run();
}

}  // namespace aurora::test_cases::utest_to_code
