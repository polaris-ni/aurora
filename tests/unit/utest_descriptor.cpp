/// 测试类型: unit
/// 目标单元: src/aurora/widget/descriptor.cpp
/// 测试说明: descriptor.cpp 提供两个序列化函数：descriptor_to_json(PropDescriptor) 与
/// descriptor_to_json(WidgetDescriptor)。两者均按「字段非空才输出」的契约生成 JSON；构造全字段填满
/// 的描述符可覆盖所有分支（含 enum/min/max/pattern/constraint/requires_props/conflicts_with 与
/// allowed_child_types/invariants），构造极简描述符可覆盖「跳过可选字段」的分支。
/// 判据：全字段描述符的 JSON 含全部非空键；极简描述符的 JSON 不含可选键。

#include "aurora/widget/descriptor.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_descriptor {

AURORA_TEST_CASE(prop_descriptor_full_fields_serializes_everything) {
    PropDescriptor p;
    p.name = "header";
    p.type = "string";
    p.default_value = "\"\"";
    p.required = true;
    p.note = "Title text";
    p.json_type = "string";
    p.enum_values = {"compact", "expanded"};
    p.min_value = "0";
    p.max_value = "100";
    p.pattern = "^[a-z]+$";
    p.constraint = "non-empty";
    p.requires_props = {"title"};
    p.conflicts_with = {"footer"};

    const Json j = aurora::descriptor_to_json(p);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "header");
    AURORA_TEST_CHECK_EQ(j.at("type")->as_or<std::string>(""), "string");
    AURORA_TEST_CHECK_EQ(j.at("default")->as_or<std::string>(""), "\"\"");
    AURORA_TEST_CHECK_TRUE(j.at("required")->as_or<bool>(false));
    AURORA_TEST_CHECK_TRUE(j.contains("note"));
    AURORA_TEST_CHECK_TRUE(j.contains("json_type"));
    AURORA_TEST_CHECK_TRUE(j.contains("enum"));
    AURORA_TEST_CHECK_TRUE(j.contains("minimum"));
    AURORA_TEST_CHECK_TRUE(j.contains("maximum"));
    AURORA_TEST_CHECK_TRUE(j.contains("pattern"));
    AURORA_TEST_CHECK_TRUE(j.contains("constraint"));
    AURORA_TEST_CHECK_TRUE(j.contains("requires_props"));
    AURORA_TEST_CHECK_TRUE(j.contains("conflicts_with"));
}

AURORA_TEST_CASE(prop_descriptor_minimal_skips_optional_keys) {
    PropDescriptor p;
    p.name = "count";
    p.type = "int";
    p.default_value = "0";
    p.required = false;

    const Json j = aurora::descriptor_to_json(p);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "count");
    AURORA_TEST_CHECK_FALSE(j.contains("note"));
    AURORA_TEST_CHECK_FALSE(j.contains("enum"));
    AURORA_TEST_CHECK_FALSE(j.contains("minimum"));
    AURORA_TEST_CHECK_FALSE(j.contains("pattern"));
    AURORA_TEST_CHECK_FALSE(j.contains("requires_props"));
}

AURORA_TEST_CASE(widget_descriptor_full_and_minimal) {
    WidgetDescriptor full;
    full.name = "ExpansionPanel";
    full.ns = "aurora";
    full.properties.push_back(
        PropDescriptor{.name = "header", .type = "string", .default_value = "\"\"", .required = true});
    full.events.push_back("on_toggle");
    full.children_policy = "single";
    full.examples.push_back("au::ExpansionPanel(\"x\", au::Text(\"y\"))");
    full.allowed_child_types = {"Text", "Image"};
    full.invariants = {"header non-empty"};

    const Json j = aurora::descriptor_to_json(full);
    AURORA_TEST_CHECK_EQ(j.at("name")->as_or<std::string>(""), "ExpansionPanel");
    AURORA_TEST_CHECK_EQ(j.at("namespace")->as_or<std::string>(""), "aurora");
    AURORA_TEST_CHECK_TRUE(j.contains("properties"));
    AURORA_TEST_CHECK_TRUE(j.contains("events"));
    AURORA_TEST_CHECK_EQ(j.at("children_policy")->as_or<std::string>(""), "single");
    AURORA_TEST_CHECK_TRUE(j.contains("examples"));
    AURORA_TEST_CHECK_TRUE(j.contains("allowed_child_types"));
    AURORA_TEST_CHECK_TRUE(j.contains("invariants"));

    // 极简：仅必填字段，扩展字段不输出。
    WidgetDescriptor minimal;
    minimal.name = "Leaf";
    const Json jm = aurora::descriptor_to_json(minimal);
    AURORA_TEST_CHECK_EQ(jm.at("name")->as_or<std::string>(""), "Leaf");
    AURORA_TEST_CHECK_FALSE(jm.contains("allowed_child_types"));
    AURORA_TEST_CHECK_FALSE(jm.contains("invariants"));
}

}  // namespace aurora::test_cases::utest_descriptor
