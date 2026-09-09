/// 测试类型: unit
/// 目标单元: include/aurora/core/error_codes.h
/// 测试说明: 生成表 AURORA_ERROR_TABLE 的索引不变量与冻结 slug 契约、slug/ident 唯一性、to_string
/// 全枚举映射、查表辅助函数一致性、format_message 渲染及边界、查表函数 constexpr 可用性（连带覆盖 error_codes.gen.h
/// 可观测行为）

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include "aurora/core/error_codes.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_error_codes {

AURORA_TEST_CASE(table_index_matches_enum_value) {
    AURORA_TEST_REQUIRE_GE(aurora::error_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(aurora::error_count(), AURORA_ERROR_TABLE.size());
    for (std::size_t i = 0; i < AURORA_ERROR_TABLE.size(); ++i) {
        // 查表函数以 static_cast<size_t>(code) 索引：枚举值与表下标错位会整体读错元数据。
        AURORA_TEST_CHECK(AURORA_ERROR_TABLE.at(i).code == static_cast<ErrorCode>(i));
    }
}

AURORA_TEST_CASE(frozen_slugs_are_stable_contracts) {
    // slug 是对外冻结标识（改名也不变），C++ 标识符（ident）仅调试用。
    AURORA_TEST_CHECK_STREQ(aurora::slug(ErrorCode::GeneralUnknown), "general-unknown");
    AURORA_TEST_CHECK_STREQ(aurora::slug(ErrorCode::NavDepthExceeded), "nav-depth-exceeded");
    AURORA_TEST_CHECK_STREQ(aurora::slug(ErrorCode::StorageIoError), "storage-io-error");
    // slug 与 ident 解耦的典型：ident 为 RuntimeAsyncTimeout，slug 为 async-timeout。
    AURORA_TEST_CHECK_STREQ(aurora::slug(ErrorCode::RuntimeAsyncTimeout), "async-timeout");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCode::RuntimeAsyncTimeout), "RuntimeAsyncTimeout");
}

AURORA_TEST_CASE(table_entries_have_unique_nonempty_keys) {
    std::set<std::string_view> slugs;
    for (const auto& entry : AURORA_ERROR_TABLE) {
        AURORA_TEST_CHECK_FALSE(entry.slug.empty());
        AURORA_TEST_CHECK_FALSE(entry.ident.empty());
        AURORA_TEST_CHECK_FALSE(entry.message_tpl.empty());
        AURORA_TEST_CHECK_FALSE(entry.hint.empty());
        AURORA_TEST_CHECK_FALSE(entry.fix_category.empty());
        slugs.insert(entry.slug);
    }
    // slug 重复即破坏对外唯一契约。
    AURORA_TEST_CHECK_EQ(slugs.size(), AURORA_ERROR_TABLE.size());
}

AURORA_TEST_CASE(category_and_severity_to_string_complete) {
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::General), "general");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Layout), "layout");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Widget), "widget");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Render), "render");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Io), "io");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Validation), "validation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Navigation), "navigation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Platform), "platform");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Runtime), "runtime");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Generation), "generation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorCategory::Diagnostic), "diagnostic");

    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Info), "info");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Warning), "warning");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Error), "error");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Fatal), "fatal");
}

AURORA_TEST_CASE(lookup_helpers_match_table_entry) {
    constexpr auto code = ErrorCode::LayoutNullChild;
    const auto& meta = AURORA_ERROR_TABLE.at(static_cast<std::size_t>(code));
    AURORA_TEST_CHECK(aurora::slug(code) == meta.slug);
    AURORA_TEST_CHECK(aurora::category(code) == meta.category && meta.category == ErrorCategory::Layout);
    AURORA_TEST_CHECK(aurora::severity(code) == meta.severity && meta.severity == ErrorSeverity::Warning);
    AURORA_TEST_CHECK_EQ(aurora::is_auto_fixable(code), meta.auto_fixable);
    AURORA_TEST_CHECK_STREQ(aurora::fix_category_of(code), meta.fix_category);
    AURORA_TEST_CHECK_EQ(aurora::retryable(code), meta.retryable);
    AURORA_TEST_CHECK_STREQ(aurora::hint_of(code), meta.hint);
    // 抽查另一条 retryable 记录。
    AURORA_TEST_CHECK_EQ(aurora::retryable(ErrorCode::RuntimeCoroutineException), true);
}

AURORA_TEST_CASE(format_message_renders_known_placeholders) {
    AURORA_TEST_CHECK_STREQ(aurora::format_message("depth limit {max} reached", ErrorParams{{"max", "8"}}),
                            "depth limit 8 reached");
    AURORA_TEST_CHECK_STREQ(aurora::format_message("no params here", {}), "no params here");
    AURORA_TEST_CHECK_STREQ(aurora::format_message("", {}), "");
    // 未知 key 不渲染，保留占位符原样。
    AURORA_TEST_CHECK_STREQ(aurora::format_message("missing {nope} key", {}), "missing {nope} key");
}

AURORA_TEST_CASE(format_message_handles_edge_cases) {
    // 未闭合的 '{'：找不到 '}' 时按原字符输出。
    AURORA_TEST_CHECK_STREQ(aurora::format_message("open {brace", {}), "open {brace");
    // 多占位符与相邻占位符。
    AURORA_TEST_CHECK_STREQ(aurora::format_message("{a}-{b}", ErrorParams{{"a", "1"}, {"b", "2"}}), "1-2");
    AURORA_TEST_CHECK_STREQ(aurora::format_message("{a}{b}", ErrorParams{{"a", "1"}, {"b", "2"}}), "12");
    // 空 key：查不到参数，原样保留。
    AURORA_TEST_CHECK_STREQ(aurora::format_message("{}", {}), "{}");
}

AURORA_TEST_CASE(lookup_functions_are_compile_time_usable) {
    // 查表函数 constexpr：冻结契约可在编译期断言。
    static_assert(aurora::error_count() == AURORA_ERROR_TABLE.size());
    static_assert(aurora::slug(ErrorCode::GeneralUnknown) == "general-unknown");
    static_assert(aurora::severity(ErrorCode::NavDepthExceeded) == ErrorSeverity::Error);
    static_assert(aurora::category(ErrorCode::LayoutNullChild) == ErrorCategory::Layout);
    static_assert(aurora::retryable(ErrorCode::RuntimeAsyncTimeout));
    static_assert(!aurora::is_auto_fixable(ErrorCode::SurfaceLost));
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_error_codes
