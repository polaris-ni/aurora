/// 测试类型: unit
/// 目标单元: include/aurora/widget/yaml.h
/// 测试说明: 覆盖 serialization::to_yaml 发射器——标量与浮点特殊值（.nan/.inf/补 .0）、
/// 字符串引号判定与转义、对象/数组/空容器/嵌套结构与缩进、键名引号，以及真实 widget 树
/// Json→YAML 冒烟

#include <limits>
#include <string>

#include "aurora/widget/serialization.h"
#include "aurora/widget/text.h"
#include "aurora/widget/yaml.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_yaml {

AURORA_TEST_CASE(yaml_scalar_emission) {
    // null / bool
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(nullptr)), "null");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(true)), "true");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(false)), "false");

    // 整数（含符号与零）
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(42)), "42");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(-7)), "-7");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(0)), "0");

    // 浮点：输出必须含小数点（YAML 浮点契约，1.0 不塌缩为整数 1）。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(1.0)), "1.0");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(2.5)), "2.5");
    const std::string pi = serialization::to_yaml(Json(3.14));
    AURORA_TEST_CHECK_MSG(pi.find("3.14") != std::string::npos, "float 3.14 output contains 3.14");
}

AURORA_TEST_CASE(yaml_float_special_values) {
    using lim = std::numeric_limits<double>;
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(lim::quiet_NaN())), ".nan");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(lim::infinity())), ".inf");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(-lim::infinity())), "-.inf");
}

AURORA_TEST_CASE(yaml_string_quoting_rules) {
    // 纯字母（含内部空格）无需引号。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("hello"))), "hello");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("hello world"))), "hello world");
    // `-` 在 YAML 特殊字符集内（块标量指示符），触发引号。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("v1.0-alpha"))), "\"v1.0-alpha\"");

    // 空串必须引号包裹。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string(""))), "\"\"");

    // 数值形态字符串必须引号（否则被 YAML 解析为数字）。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("700"))), "\"700\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("1.5"))), "\"1.5\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("-42"))), "\"-42\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("+3"))), "\"+3\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("1e5"))), "\"1e5\"");

    // 布尔/null 字面量字符串必须引号。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("true"))), "\"true\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("False"))), "\"False\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("null"))), "\"null\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("no"))), "\"no\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("ON"))), "\"ON\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("~"))), "\"~\"");

    // 含 YAML 特殊字符必须引号。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("key: value"))), "\"key: value\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("#comment"))), "\"#comment\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("a,b"))), "\"a,b\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("[x]"))), "\"[x]\"");

    // 前后空白必须引号。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string(" padded "))), "\" padded \"");

    // 引号/反斜杠转义。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("say \"hi\""))), "\"say \\\"hi\\\"\"");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("a\\b"))), "\"a\\\\b\"");
    // 实现现状：引号判定只看首尾空白与特殊字符集，字符串内部的控制字符（\n/\t）
    // 既不触发引号也不转义，原样落盘——已知语义，按代码运行时为准锁定。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("l\nr"))), "l\nr");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json(std::string("x\ty"))), "x\ty");
}

AURORA_TEST_CASE(yaml_object_array_and_empty) {
    // 空容器。
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json::object()), "{}");
    AURORA_TEST_CHECK_EQ(serialization::to_yaml(Json::array()), "[]");

    // 扁平对象。
    Json obj = Json::object();
    obj["name"] = "Aurora";
    obj["version"] = 1;
    const std::string o = serialization::to_yaml(obj);
    AURORA_TEST_CHECK_MSG(o.find("name: Aurora") != std::string::npos, "object emits key: unquoted value");
    AURORA_TEST_CHECK_MSG(o.find("version: 1") != std::string::npos, "object emits key: integer value");

    // 标量数组：- item 直接跟在键后。
    Json arr = Json::array({1, 2, 3});
    const std::string a = serialization::to_yaml(arr);
    AURORA_TEST_CHECK_MSG(a.find("- 1") != std::string::npos, "array emits dash item");
    AURORA_TEST_CHECK_MSG(a.find("- 3") != std::string::npos, "array emits last dash item");

    Json with_items = Json::object();
    with_items["items"] = Json::array({1, 2});
    const std::string w = serialization::to_yaml(with_items);
    AURORA_TEST_CHECK_MSG(w.find("items:") != std::string::npos, "scalar array key on its own line");
    AURORA_TEST_CHECK_MSG(w.find("- 1") != std::string::npos, "scalar array items follow key");

    // 键名按值语义加引号（"123" 会被解析为整数，必须引号）。
    Json keyed = Json::object();
    keyed["123"] = 1;
    keyed["true"] = 2;
    const std::string k = serialization::to_yaml(keyed);
    AURORA_TEST_CHECK_MSG(k.find("\"123\": 1") != std::string::npos, "numeric key must be quoted");
    AURORA_TEST_CHECK_MSG(k.find("\"true\": 2") != std::string::npos, "boolean-like key must be quoted");
}

AURORA_TEST_CASE(yaml_nested_structure_and_indent) {
    Json inner = Json::object();
    inner["x"] = 10;
    inner["y"] = 20;
    Json j = Json::object();
    j["title"] = "Test";
    j["pos"] = inner;

    const std::string s = serialization::to_yaml(j);
    AURORA_TEST_CHECK_MSG(s.find("title: Test") != std::string::npos, "top-level scalar key");
    AURORA_TEST_CHECK_MSG(s.find("pos:") != std::string::npos, "nested object key");
    AURORA_TEST_CHECK_MSG(s.find("  x: 10") != std::string::npos, "inner key indented 2 spaces");
    AURORA_TEST_CHECK_MSG(s.find("  y: 20") != std::string::npos, "inner key indented 2 spaces");

    // 值为空容器时内联输出。
    Json e = Json::object();
    e["empty_arr"] = Json::array();
    e["empty_obj"] = Json::object();
    const std::string es = serialization::to_yaml(e);
    AURORA_TEST_CHECK_MSG(es.find("empty_arr: []") != std::string::npos, "empty array inline");
    AURORA_TEST_CHECK_MSG(es.find("empty_obj: {}") != std::string::npos, "empty object inline");

    // 对象数组：首行 dash 缩进挂在键下。
    Json member = Json::object();
    member["a"] = 1;
    Json list = Json::object();
    list["list"] = Json::array({member});
    const std::string ls = serialization::to_yaml(list);
    AURORA_TEST_CHECK_MSG(ls.find("list:") != std::string::npos, "object array key on its own line");
    AURORA_TEST_CHECK_MSG(ls.find("- ") != std::string::npos, "object array emits dash");
    AURORA_TEST_CHECK_MSG(ls.find("a: 1") != std::string::npos, "object array member fields");

    // indent 参数整体平移缩进（indent=1 → 顶层行前 2 空格）。
    Json one = Json::object();
    one["name"] = "Aurora";
    const std::string padded = serialization::to_yaml(one, 1);
    AURORA_TEST_CHECK_MSG(padded.find("  name: Aurora") == 0, "indent shifts top-level lines");
}

AURORA_TEST_CASE(yaml_widget_tree_smoke) {
    // 真实 widget 树 → to_json → to_yaml 全链路冒烟。
    Node root{Text{"Hello"}};
    const Json j = serialization::to_json(root.widget());
    const std::string y = serialization::to_yaml(j);
    AURORA_TEST_CHECK_MSG(y.find("type: Text") != std::string::npos, "widget yaml contains node type");
    AURORA_TEST_CHECK_MSG(y.find("content: Hello") != std::string::npos, "widget yaml contains text content");
}

}  // namespace aurora::test_cases::utest_yaml
