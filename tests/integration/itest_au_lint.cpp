/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: 驱动 aurora_lint 的结构化检查核心（tools/include/au_lint_core.h 的 lint_ui_tree），
///           验证合法树零 error、未知类型/未知属性告警、缺 type/children 非数组报错、空容器提示
/// 覆盖说明: aurora_lint 是独立可执行，库 API（list_all_components/describe_component）直测为主

#include <algorithm>
#include <string>
#include <vector>

#include "au_lint_core.h"
#include "aurora/aurora.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_au_lint {

using au::serialization::register_core_widgets;
using au::tools::lint_ui_tree;
using au::tools::LintFinding;

namespace {

auto count_code(const std::vector<LintFinding> &findings, std::string_view code) -> int {
    int n = 0;
    for (const auto &x : findings) {
        if (x.code == code) {
            ++n;
        }
    }
    return n;
}

auto has_error(const std::vector<LintFinding> &findings) -> bool {
    return std::ranges::any_of(findings,
                               [](const LintFinding &x) -> bool { return x.severity == au::ErrorSeverity::Error; });
}

}  // namespace

AURORA_TEST_CASE(lint_valid_tree_has_no_error_findings) {
    register_core_widgets();  // aurora_lint 主程序依赖全局静态注册，测试显式注册
    au::Json ok = au::Json::object();
    ok["type"] = "Column";
    ok["props"] = au::Json::object();
    au::Json child = au::Json::object();
    child["type"] = "Text";
    child["props"] = au::Json{{"content", "hi"}};
    ok["children"] = au::Json::array({child});

    const auto findings = lint_ui_tree(ok);
    AURORA_TEST_CHECK_MSG(!has_error(findings), "valid tree should have no error-level findings");
}

AURORA_TEST_CASE(lint_unknown_type_is_warning_not_error) {
    register_core_widgets();
    au::Json unk = au::Json::object();
    unk["type"] = "BogusWidget";
    unk["props"] = au::Json::object();

    const auto findings = lint_ui_tree(unk);
    AURORA_TEST_CHECK_MSG(count_code(findings, "unknown-type") == 1, "unknown type yields unknown-type warning");
    AURORA_TEST_CHECK_MSG(!has_error(findings), "unknown-type is a warning, not an error");
}

AURORA_TEST_CASE(lint_missing_type_is_error) {
    register_core_widgets();
    au::Json notype = au::Json::object();
    notype["props"] = au::Json::object();

    const auto findings = lint_ui_tree(notype);
    AURORA_TEST_CHECK_MSG(count_code(findings, "node-no-type") == 1, "missing type yields node-no-type error");
    AURORA_TEST_CHECK_MSG(has_error(findings), "node-no-type is error level");
}

AURORA_TEST_CASE(lint_children_not_array_is_error) {
    register_core_widgets();
    au::Json badch = au::Json::object();
    badch["type"] = "Column";
    badch["children"] = "oops";

    const auto findings = lint_ui_tree(badch);
    AURORA_TEST_CHECK_MSG(count_code(findings, "children-not-array") == 1,
                          "non-array children yields children-not-array error");
}

AURORA_TEST_CASE(lint_unknown_prop_is_warning) {
    register_core_widgets();
    au::Json badprop = au::Json::object();
    badprop["type"] = "Text";
    badprop["props"] = au::Json{{"__nope__", 1}};

    const auto findings = lint_ui_tree(badprop);
    AURORA_TEST_CHECK_MSG(count_code(findings, "unknown-prop") >= 1, "unknown prop yields unknown-prop warning");
}

AURORA_TEST_CASE(lint_empty_container_is_info) {
    register_core_widgets();
    au::Json empty = au::Json::object();
    empty["type"] = "Column";
    empty["props"] = au::Json::object();
    empty["children"] = au::Json::array();

    const auto findings = lint_ui_tree(empty);
    AURORA_TEST_CHECK_MSG(count_code(findings, "empty-container") == 1,
                          "empty children yields empty-container info");
    AURORA_TEST_CHECK_MSG(!has_error(findings), "empty-container is info level, not an error");
}

}  // namespace aurora::test_cases::itest_au_lint
