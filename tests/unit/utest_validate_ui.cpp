/// 测试类型: unit
/// 目标单元: include/aurora/app/validate_ui.h
/// 测试说明: 覆盖 UI 树 JSON Schema 校验——合法树放行与 JSON 报告形态、非对象/缺 type 拒绝、
/// 未知类型、缺失必填属性（Button.label / Text.content）、属性类型不匹配（Text.content）、
/// children_policy none/single 违规、嵌套超限、ValidationError::to_json 结构

#include <string>
#include <vector>

#include "aurora/app/validate_ui.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_validate_ui {

namespace {

/// 在错误清单中按 JSON 路径查找（未命中返回 nullptr）。
auto find_error(const std::vector<ValidationError> &errs, const std::string &path) -> const ValidationError * {
    for (const auto &e : errs) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

/// 可 from_json 的最小 Text 节点（schema 必填 prop 为 content，见 aurora_api.json Text 条目）。
auto text_node(const char *content) -> Json {
    Json j;
    j["type"] = "Text";
    j["props"]["content"] = content;
    return j;
}

/// 逐层嵌套 levels 层的 Column JSON 树。
auto deep_tree(int levels) -> Json {
    Json root;
    root["type"] = "Column";
    Json *cur = &root;
    for (int i = 0; i < levels; ++i) {
        Json child;
        child["type"] = "Column";
        (*cur)["children"] = Json::array({child});
        cur = &(*cur)["children"][0];
    }
    return root;
}

}  // namespace

AURORA_TEST_CASE(valid_tree_passes_and_report_json_true) {
    Json tree;
    tree["type"] = "Column";
    tree["children"] = Json::array({text_node("hi")});

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_CHECK_TRUE(errs.empty());

    // 便捷报告：valid=true + 空错误数组。
    const auto report = validate_ui_tree_json(tree);
    AURORA_TEST_CHECK_EQ(report["valid"].get<bool>(), true);
    AURORA_TEST_CHECK_TRUE(report["errors"].empty());
}

AURORA_TEST_CASE(non_object_or_missing_type_rejected) {
    // 节点不是对象（空数组）。
    const auto as_array = validate_ui_tree(Json::array());
    AURORA_TEST_REQUIRE_EQ(as_array.size(), 1U);
    AURORA_TEST_CHECK_STREQ(as_array[0].path, "$");
    AURORA_TEST_CHECK_TRUE(as_array[0].message.find("must be a JSON object") != std::string::npos);

    // 对象但缺 "type" 字段。
    const auto no_type = validate_ui_tree(Json::object());
    AURORA_TEST_REQUIRE_EQ(no_type.size(), 1U);
    AURORA_TEST_CHECK_STREQ(no_type[0].path, "$");
    AURORA_TEST_CHECK_TRUE(no_type[0].message.find("type") != std::string::npos);
}

AURORA_TEST_CASE(unknown_widget_type_reported) {
    Json tree;
    tree["type"] = "NoSuchWidgetXyz";

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_REQUIRE_EQ(errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(errs[0].path, "$.type");
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("unknown widget type") != std::string::npos);
    // 建议携带已知类型列表。
    AURORA_TEST_CHECK_TRUE(errs[0].suggestion.rfind("use one of:", 0) == 0);
}

AURORA_TEST_CASE(missing_required_prop_reported) {
    Json tree;
    tree["type"] = "Button";
    tree["props"] = Json::object();

    const auto errs = validate_ui_tree(tree);
    const auto *e = find_error(errs, "$.props.label");
    AURORA_TEST_REQUIRE_NOT_NULL(e);
    AURORA_TEST_CHECK_TRUE(e->message.find("missing required prop") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(e->message.find("label") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(e->suggestion.find("label") != std::string::npos);
}

AURORA_TEST_CASE(prop_type_mismatch_reported) {
    Json tree = text_node("x");
    tree["props"]["content"] = 123;  // 声明为字符串（LocalizedString），给了数字

    const auto errs = validate_ui_tree(tree);
    const auto *e = find_error(errs, "$.props.content");
    AURORA_TEST_REQUIRE_NOT_NULL(e);
    AURORA_TEST_CHECK_TRUE(e->message.find("type mismatch") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(e->message.find("number") != std::string::npos);
}

AURORA_TEST_CASE(children_policy_violations_reported) {
    // children_policy=none：Text 不接受子节点。
    Json leaf = text_node("x");
    Json text_with_children = text_node("parent");
    text_with_children["children"] = Json::array({leaf});
    const auto none_errs = validate_ui_tree(text_with_children);
    AURORA_TEST_REQUIRE_EQ(none_errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(none_errs[0].path, "$.children");
    AURORA_TEST_CHECK_TRUE(none_errs[0].message.find("does not accept children") != std::string::npos);

    // children_policy=single：PerfOverlay 最多 1 个子节点。
    Json overlay;
    overlay["type"] = "PerfOverlay";
    overlay["children"] = Json::array({text_node("a"), text_node("b")});
    const auto single_errs = validate_ui_tree(overlay);
    AURORA_TEST_REQUIRE_EQ(single_errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(single_errs[0].path, "$.children");
    AURORA_TEST_CHECK_TRUE(single_errs[0].message.find("accepts at most 1") != std::string::npos);
}

AURORA_TEST_CASE(deep_nesting_reported) {
    const auto tree = deep_tree(70);  // 超过默认上限 64

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_REQUIRE_FALSE(errs.empty());
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("nesting depth exceeded") != std::string::npos);
    // 路径指向深层 children 链。
    AURORA_TEST_CHECK_TRUE(errs[0].path.rfind("$.children", 0) == 0);
}

AURORA_TEST_CASE(validation_error_json_shape) {
    ValidationError e;
    e.path = "$.props.text";
    e.message = "boom";
    e.suggestion = "";
    auto j = e.to_json();
    AURORA_TEST_CHECK_STREQ(j["path"].get<std::string>(), "$.props.text");
    AURORA_TEST_CHECK_STREQ(j["message"].get<std::string>(), "boom");
    // 空建议不落盘。
    AURORA_TEST_CHECK_FALSE(j.contains("suggestion"));

    e.suggestion = "fix it";
    j = e.to_json();
    AURORA_TEST_CHECK_STREQ(j["suggestion"].get<std::string>(), "fix it");
}

}  // namespace aurora::test_cases::utest_validate_ui
