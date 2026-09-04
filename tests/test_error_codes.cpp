// test_error_codes.cpp — 覆盖 errors.toml 单一声明源生成的错误体系：
// 枚举连续性、slug 冻结契约、表驱动元数据、make_error 表驱动、Diagnostic 表驱动解释。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// core/error_codes.h（含 error_codes.gen.h 生成枚举/表驱动元数据）。

#include <string>
#include <unordered_set>

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "test_harness.h"

using aurora::Diagnostics;
using aurora::error_count;
using aurora::ErrorCategory;
using aurora::ErrorCode;
using aurora::ErrorSeverity;

static void test_enum_contiguous() {
    // ErrorCode 必须为 0 起连续索引，g_error_table 才能按下标查表。
    auto prev = ErrorCode::GeneralUnknown;
    std::size_t count = 0;
    for (std::size_t i = 0; i < error_count(); ++i) {
        auto cur = static_cast<ErrorCode>(i);
        AURORA_TEST_CHECK_MSG(static_cast<std::size_t>(cur) == i, "ErrorCode contiguous indices");
        (void)prev;
        prev = cur;
        ++count;
    }
    AURORA_TEST_CHECK_MSG(count == error_count(), "error_count matches iteration");
}

static void test_slug_frozen_and_unique() {
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < error_count(); ++i) {
        const auto c = static_cast<ErrorCode>(i);
        auto s = std::string(slug(c));
        AURORA_TEST_CHECK_MSG(!s.empty(), "slug non-empty");
        AURORA_TEST_CHECK_MSG(!seen.contains(s), "slug unique");
        seen.insert(s);
        // to_string 返回 C++ 标识符（调试用），不应等于 slug
        AURORA_TEST_CHECK_MSG(std::string(to_string(c)) != s, "to_string(enum) != slug");
    }
}

static void test_table_metadata() {
    AURORA_TEST_CHECK_MSG(category(ErrorCode::LayoutDepthExceeded) == ErrorCategory::Layout, "category from table");
    AURORA_TEST_CHECK_MSG(severity(ErrorCode::LayoutDepthExceeded) == ErrorSeverity::Warning, "severity from table");
    AURORA_TEST_CHECK_MSG(is_auto_fixable(ErrorCode::LayoutDepthExceeded), "auto_fixable from table");
    AURORA_TEST_CHECK_MSG(!is_auto_fixable(ErrorCode::NavDepthExceeded), "auto_fixable(false) from table");
    AURORA_TEST_CHECK_MSG(std::string(fix_category_of(ErrorCode::LayoutDepthExceeded)) == "layout_conflict",
                          "fix_category from table");
    AURORA_TEST_CHECK_MSG(retryable(ErrorCode::RuntimeAsyncTimeout), "retryable from table");
    AURORA_TEST_CHECK_MSG(std::string(to_string(ErrorCode::NavDepthExceeded)) == "NavDepthExceeded",
                          "to_string returns identifier");
    AURORA_TEST_CHECK_MSG(std::string(slug(ErrorCode::NavDepthExceeded)) == "nav-depth-exceeded",
                          "slug is frozen external code");
}

static void test_make_error_table_driven() {
    auto e = make_error(ErrorCode::LayoutDepthExceeded, {{"max", "10"}});
    AURORA_TEST_CHECK_MSG(e.code == "layout-depth-exceeded", "make_error: code is slug");
    AURORA_TEST_CHECK_MSG(e.code_enum == ErrorCode::LayoutDepthExceeded, "make_error: code_enum");
    AURORA_TEST_CHECK_MSG(e.message == "Layout tree depth exceeded the limit (default 10)",
                          "make_error: template-rendered message");
    AURORA_TEST_CHECK_MSG(e.severity == ErrorSeverity::Warning, "make_error: injected severity");
    AURORA_TEST_CHECK_MSG(e.category == ErrorCategory::Layout, "make_error: injected category");
    AURORA_TEST_CHECK_MSG(e.auto_fixable, "make_error: injected auto_fixable");
    AURORA_TEST_CHECK_MSG(!e.hint.empty(), "make_error: injected default hint");

    // 自定义 message 覆盖模板
    auto e2 = make_error(ErrorCode::GeneralUnknown, "自定义");
    AURORA_TEST_CHECK_MSG(e2.message == "自定义" && e2.code == "general-unknown", "make_error: custom message");

    // hint 可被覆盖
    auto e3 = make_error(ErrorCode::NavDepthExceeded, "m", {{"max", "5"}}, "覆盖提示");
    AURORA_TEST_CHECK_MSG(e3.hint == "覆盖提示", "make_error: hint overridable");
}

static void test_diagnostic_explain_table_driven() {
    // 诊断码即 slug，解释文本来自 ErrorMeta.hint（单一声明源）
    const std::string hint = Diagnostics::explain_diagnostic("widget-depth-exceeded");
    AURORA_TEST_CHECK_MSG(!hint.empty() && hint.find("Repeater") != std::string::npos, "explain(slug) from table");
    const std::string hint2 = Diagnostics::explain_diagnostic(ErrorCode::FontMissing);
    AURORA_TEST_CHECK_MSG(!hint2.empty() && hint2.find("font") != std::string::npos, "explain(enum) from table");
    // 未知码返回兜底
    AURORA_TEST_CHECK_MSG(!Diagnostics::explain_diagnostic("no-such-code").empty(), "explain unknown code fallback");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== error_codes ===\n");
    test_enum_contiguous();
    test_slug_frozen_and_unique();
    test_table_metadata();
    test_make_error_table_driven();
    test_diagnostic_explain_table_driven();
}
