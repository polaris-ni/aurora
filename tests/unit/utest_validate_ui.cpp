/// 测试类型: unit
/// 目标单元: include/aurora/app/validate_ui.h
/// 测试说明: 覆盖 UI 树 JSON Schema 校验——合法树放行与 JSON 报告形态、非对象/缺 type 拒绝、
/// 未知类型、缺失必填属性（Button.label / Text.content）、属性类型不匹配（Text.content）、
/// children_policy none/single 违规、嵌套超限、ValidationError::to_json 结构

#include <string>
#include <vector>

#include "aurora/app/validate_ui.h"
#include "aurora/icon/icon.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "framework/json_literals.h"

namespace aurora::test_cases::utest_validate_ui {
using aurora::testing::require_child;
using aurora::testing::require_field;

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
    Json j = Json::object();
    j.set("type", "Text");
    j.set("props", testing::json_obj({{"content", content}}));
    return j;
}

/// 逐层嵌套 levels 层的 Column JSON 树。
auto deep_tree(int levels) -> Json {
    Json root = Json::object();
    root.set("type", "Column");
    Json *cur = &root;
    for (int i = 0; i < levels; ++i) {
        Json child = Json::object();
        child.set("type", "Column");
        cur->set("children", testing::json_arr({child}));
        cur = cur->at("children")->at(0);
    }
    return root;
}

}  // namespace

AURORA_TEST_CASE(valid_tree_passes_and_report_json_true) {
    Json tree = Json::object();
    tree.set("type", "Column");
    tree.set("children", testing::json_arr({text_node("hi")}));

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_CHECK_TRUE(errs.empty());

    // 便捷报告：valid=true + 空错误数组。
    const auto report = validate_ui_tree_json(tree);
    AURORA_TEST_CHECK_EQ(require_field<bool>(report, "valid"), true);
    AURORA_TEST_CHECK_TRUE(require_child(report, "errors")->empty());
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
    Json tree = Json::object();
    tree.set("type", "NoSuchWidgetXyz");

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_REQUIRE_EQ(errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(errs[0].path, "$.type");
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("unknown widget type") != std::string::npos);
    // 建议携带已知类型列表。
    AURORA_TEST_CHECK_TRUE(errs[0].suggestion.rfind("use one of:", 0) == 0);
}

AURORA_TEST_CASE(missing_required_prop_reported) {
    Json tree = Json::object();
    tree.set("type", "Button");
    tree.set("props", Json::object());

    const auto errs = validate_ui_tree(tree);
    const auto *e = find_error(errs, "$.props.label");
    AURORA_TEST_REQUIRE_NOT_NULL(e);
    AURORA_TEST_CHECK_TRUE(e->message.find("missing required prop") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(e->message.find("label") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(e->suggestion.find("label") != std::string::npos);
}

AURORA_TEST_CASE(prop_type_mismatch_reported) {
    Json tree = text_node("x");
    tree.at("props")->set("content", 123);  // 声明为字符串（LocalizedString），给了数字

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
    text_with_children.set("children", testing::json_arr({leaf}));
    const auto none_errs = validate_ui_tree(text_with_children);
    AURORA_TEST_REQUIRE_EQ(none_errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(none_errs[0].path, "$.children");
    AURORA_TEST_CHECK_TRUE(none_errs[0].message.find("does not accept children") != std::string::npos);

    // children_policy=single：PerfOverlay 最多 1 个子节点。
    Json overlay = Json::object();
    overlay.set("type", "PerfOverlay");
    overlay.set("children", testing::json_arr({text_node("a"), text_node("b")}));
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
    AURORA_TEST_CHECK_STREQ(require_field<std::string>(j, "path"), "$.props.text");
    AURORA_TEST_CHECK_STREQ(require_field<std::string>(j, "message"), "boom");
    // 空建议不落盘。
    AURORA_TEST_CHECK_FALSE(j.contains("suggestion"));

    e.suggestion = "fix it";
    j = e.to_json();
    AURORA_TEST_CHECK_STREQ(require_field<std::string>(j, "suggestion"), "fix it");
}

// props 字段存在但不是对象 -> 报错（覆盖 validate_node 的 props 类型守卫分支）。
AURORA_TEST_CASE(props_not_object_reports_error) {
    Json tree = Json::object();
    tree.set("type", "Text");
    tree.set("props", Json{"not-an-object"});

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_REQUIRE_FALSE(errs.empty());
    bool hit = false;
    for (const auto &e : errs) {
        if (e.message.find("\"props\" must be a JSON object") != std::string::npos) {
            hit = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(hit);
}

// children 字段存在但不是数组 -> 报错（覆盖 children 类型守卫分支）。
AURORA_TEST_CASE(children_not_array_reports_error) {
    Json tree = Json::object();
    tree.set("type", "Text");
    tree.set("props", testing::json_obj({{"content", "x"}}));
    tree.set("children", "not-an-array");

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_REQUIRE_EQ(errs.size(), 1U);
    AURORA_TEST_CHECK_STREQ(errs[0].path, "$.children");
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("\"children\" must be an array") != std::string::npos);
}

// DataTable.row_count 给定六种错误类型 -> 覆盖 type_matches 不匹配路径与 json_type_name 全部分支。
AURORA_TEST_CASE(type_mismatch_classifies_every_json_type) {
    const std::vector<std::pair<std::string, Json>> bad{
        {"string", Json{"not-an-int"}}, {"number", Json{1.5}},    {"bool", Json{true}},
        {"object", Json::object()},     {"array", Json::array()}, {"null", Json{nullptr}},
    };
    for (const auto &[kind, value] : bad) {
        Json tree = Json::object();
        tree.set("type", "DataTable");
        Json props = Json::object();
        props.set("columns", Json::array());  // 必填，给合法空数组
        props.set("row_count", value);
        tree.set("props", std::move(props));

        const auto errs = validate_ui_tree(tree);
        AURORA_TEST_REQUIRE_FALSE(errs.empty());
        AURORA_TEST_CHECK_TRUE(errs[0].message.find("type mismatch") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(errs[0].message.find(kind) != std::string::npos);
    }
}

// 合法 Text 带 bool/float/未知类型化属性 -> 覆盖 type_matches 的 bool / float / 未知类型放行分支。
AURORA_TEST_CASE(valid_text_typed_props_exercise_type_matches) {
    Json tree = Json::object();
    tree.set("type", "Text");
    Json props = Json::object();
    props.set("content", "hi");  // LocalizedString -> string 分支
    props.set("soft_wrap", Json{true});  // bool 分支
    props.set("line_height", Json{1.5});  // float 分支
    props.set("overflow", "Clip");  // TextOverflow -> 未知类型放行分支
    tree.set("props", std::move(props));

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_CHECK_TRUE(errs.empty());
}

// ---- 图标语义名校验（validate_ui 接 IconRegistry）----

AURORA_TEST_CASE(icon_semantic_name_skipped_when_registry_empty) {
    icon::IconRegistry::clear();  // 空注册表：跳过语义名校验（应用尚未注册，不误报）

    Json tree = Json::object();
    tree.set("type", "Column");
    Json icon_node = Json::object();
    Json icon_props = Json::object();
    icon_props.set("icon", "settings");
    icon_node.set("type", "Icon");
    icon_node.set("props", icon_props);
    tree.set("children", testing::json_arr({icon_node}));

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_CHECK_TRUE(errs.empty());
}

AURORA_TEST_CASE(icon_semantic_name_typo_reported_with_candidates) {
    icon::IconRegistry::clear();
    icon::IconRegistry::register_icon("settings", icon::IconData::glyph("MS", 0xE8B8));
    icon::IconRegistry::register_icon("close", icon::IconData::glyph("MS", 0xE5CD));

    Json tree = Json::object();
    tree.set("type", "Column");
    Json good = Json::object();
    Json good_props = Json::object();
    good_props.set("icon", "close");
    good.set("type", "IconButton");
    good.set("props", good_props);
    Json bad = Json::object();
    Json bad_props = Json::object();
    bad_props.set("icon", "settingz");
    bad.set("type", "Icon");
    bad.set("props", bad_props);
    tree.set("children", testing::json_arr({good, bad}));

    const auto errs = validate_ui_tree(tree);
    AURORA_TEST_CHECK_EQ(errs.size(), static_cast<std::size_t>(1));
    AURORA_TEST_CHECK_EQ(errs[0].path, "$.children[1].props.icon");
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("settingz") != std::string::npos);
    // 候选名回带：message 含已注册名，供 AI 与调用方纠错。
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("close") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(errs[0].message.find("settings") != std::string::npos);

    icon::IconRegistry::clear();  // 还原进程级表，不污染后续用例
}

}  // namespace aurora::test_cases::utest_validate_ui
