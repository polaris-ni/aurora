/// 测试类型: unit
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: au_lint 单元测试
///

// 覆盖 aurora_lint 的结构化检查核心（tools/include/au_lint_core.h）。
// 通过 lint_ui_tree() 直接驱动，验证各类结构问题的诊断码与严重级，避免重复实现漂移。
#include <algorithm>
#include <string>
#include <vector>

#include "au_lint_core.h"
#include "aurora/aurora.h"
#include "aurora/widget/serialization.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_au_lint {

using au::serialization::register_core_widgets;
using au::tools::lint_ui_tree;
using au::tools::LintFinding;

static auto count_code(const std::vector<LintFinding> &f, const char *code) -> int {
    int n = 0;
    for (const auto &x : f) {
        if (x.code == code) {
            ++n;
        }
    }
    return n;
}

static auto has_error(const std::vector<LintFinding> &f) -> bool {
    return std::ranges::any_of(f, [](const LintFinding &x) -> bool { return x.severity == au::ErrorSeverity::Error; });
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== au_lint ===\n");

    // 确保组件已注册（aurora_lint 主程序依赖全局静态注册，测试显式注册）。
    register_core_widgets();

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）

    // ---- 合法树：不应有 error 级发现 ----
    Json ok;
    ok["type"] = "Column";
    ok["props"] = Json::object();
    Json child;
    child["type"] = "Text";
    child["props"]["content"] = "hi";
    ok["children"] = Json::array({child});
    const auto r_ok = lint_ui_tree(ok);
    AURORA_TEST_CHECK_MSG(!has_error(r_ok), "valid tree should have no error-level findings");

    // ---- 未知类型：warning unknown-type（不计 error）----
    Json unk;
    unk["type"] = "BogusWidget";
    unk["props"] = Json::object();
    const auto r_unk = lint_ui_tree(unk);
    AURORA_TEST_CHECK_MSG(count_code(r_unk, "unknown-type") == 1, "unknown type yields unknown-type warning");
    AURORA_TEST_CHECK_MSG(!has_error(r_unk), "unknown-type is a warning, not an error");

    // ---- 缺少 type：error node-no-type ----
    Json notype;
    notype["props"] = Json::object();
    const auto r_notype = lint_ui_tree(notype);
    AURORA_TEST_CHECK_MSG(count_code(r_notype, "node-no-type") == 1, "missing type yields node-no-type error");
    AURORA_TEST_CHECK_MSG(has_error(r_notype), "node-no-type is error level");

    // ---- children 非数组：error children-not-array ----
    Json badch;
    badch["type"] = "Column";
    badch["children"] = "oops";
    const auto r_badch = lint_ui_tree(badch);
    AURORA_TEST_CHECK_MSG(count_code(r_badch, "children-not-array") == 1,
                          "non-array children yields children-not-array error");

    // ---- 未知属性：warning unknown-prop ----
    Json badprop;
    badprop["type"] = "Text";
    badprop["props"]["__nope__"] = 1;
    const auto r_badprop = lint_ui_tree(badprop);
    AURORA_TEST_CHECK_MSG(count_code(r_badprop, "unknown-prop") >= 1, "unknown prop yields unknown-prop warning");

    // ---- 空容器：info empty-container ----
    Json empty;
    empty["type"] = "Column";
    empty["props"] = Json::object();
    empty["children"] = Json::array();

    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto r_empty = lint_ui_tree(empty);
    AURORA_TEST_CHECK_MSG(count_code(r_empty, "empty-container") == 1, "empty children yields empty-container info");
}

}  // namespace aurora::test_cases::utest_au_lint