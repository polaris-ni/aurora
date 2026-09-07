/// 测试类型: unit
/// 目标单元: include/aurora/widget/yaml.h
/// 测试说明: utest_to_yaml 单元测试
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

// （自 utest_serialization.cpp 拆分：to_yaml 发射器段）

namespace aurora::test_cases::utest_to_yaml {

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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["name"] = "Aurora";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["version"] = 1;

    const std::string s = to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("name: Aurora") != std::string::npos, "object key: unquoted string value");
    AURORA_TEST_CHECK_MSG(s.find("version: 1") != std::string::npos, "object key: integer value");
}

// ---- 数组输出 ----
static void test_array() {
    const Json j = Json::array({1, 2, 3});
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["title"] = "Test";
    Json inner = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    inner["x"] = 10;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    inner["y"] = 20;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["pos"] = inner;

    const std::string s = to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("title: Test") != std::string::npos, "nested: top-level key");
    AURORA_TEST_CHECK_MSG(s.find("pos:") != std::string::npos, "nested: inner object key");
    AURORA_TEST_CHECK_MSG(s.find("x: 10") != std::string::npos, "nested: inner key x");
    AURORA_TEST_CHECK_MSG(s.find("y: 20") != std::string::npos, "nested: inner key y");
}

// ---- 完整 widget 树 YAML 输出 ----
static void test_widget_yaml() {
    Node root = Column{Text{"Hello"}, Button{"OK"}};
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
}  // namespace aurora::tests::sec_to_yaml

AURORA_TEST() {
    aurora::tests::sec_to_yaml::run();
}

}  // namespace aurora::test_cases::utest_to_yaml
