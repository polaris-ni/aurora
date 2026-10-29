/// 测试类型: unit
/// 目标单元: include/aurora/core/error_codes.h
/// 测试说明: 生成表 AURORA_ERROR_TABLE 的索引不变量与冻结 slug 契约、slug/ident 唯一性、domain/kind/severity
/// 三枚枚举的 to_string 全映射与两轴覆盖度、查表辅助函数一致性、format_message 渲染及边界、查表函数 constexpr
/// 可用性，以及重排后仍须成立的零值哨兵不变量（连带覆盖 error_codes.gen.h 可观测行为）

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
    for (const auto &entry : AURORA_ERROR_TABLE) {
        AURORA_TEST_CHECK_FALSE(entry.slug.empty());
        AURORA_TEST_CHECK_FALSE(entry.ident.empty());
        AURORA_TEST_CHECK_FALSE(entry.message_tpl.empty());
        AURORA_TEST_CHECK_FALSE(entry.hint.empty());
        slugs.insert(entry.slug);
    }
    // slug 重复即破坏对外唯一契约。
    AURORA_TEST_CHECK_EQ(slugs.size(), AURORA_ERROR_TABLE.size());
}

AURORA_TEST_CASE(domain_kind_and_severity_to_string_complete) {
    // 29 域：to_string 必须逐枚给出 errors.toml 里的域名，越界一律 "unknown"。
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::General), "general");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Layout), "layout");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Widget), "widget");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Render), "render");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Navigation), "navigation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Validation), "validation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Generation), "generation");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Runtime), "runtime");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Io), "io");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Json), "json");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Compression), "compression");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Storage), "storage");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::I18n), "i18n");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Prefs), "prefs");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Icon), "icon");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Platform), "platform");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Window), "window");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Dialog), "dialog");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Clipboard), "clipboard");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Drag), "drag");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Menu), "menu");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Notification), "notification");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::RecentDocuments), "recent-documents");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Webview), "webview");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Audio), "audio");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Cli), "cli");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Net), "net");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Http), "http");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorDomain::Ws), "ws");

    // 12 性质：声明序即判型优先级，to_string 须逐枚给出短横线名。
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Invalid), "invalid");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::NotFound), "not-found");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Unsupported), "unsupported");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Unavailable), "unavailable");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Denied), "denied");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Conflict), "conflict");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Exhausted), "exhausted");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Timeout), "timeout");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Cancelled), "cancelled");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Corrupt), "corrupt");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::IO), "io");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorKind::Internal), "internal");

    // 越界兜底：枚举底层是 uint8_t，表外取值不得读出相邻条目的名字。
    // 越界取值正是本用例被测目标（验证两枚 to_string 尾部的兜底返回），不可改为合法枚举值。
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto bogus_domain = static_cast<ErrorDomain>(255);
    AURORA_TEST_CHECK_STREQ(aurora::to_string(bogus_domain), "unknown");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto bogus_kind = static_cast<ErrorKind>(255);
    AURORA_TEST_CHECK_STREQ(aurora::to_string(bogus_kind), "unknown");

    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Info), "info");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Warning), "warning");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Error), "error");
    AURORA_TEST_CHECK_STREQ(aurora::to_string(ErrorSeverity::Fatal), "fatal");
}

AURORA_TEST_CASE(every_domain_and_kind_is_used_by_table_entries) {
    // 两轴各自至少有一枚条目使用：拆分的意义是「域说去哪看、性质说坏了什么」，
    // 任一枚枚举无人使用即说明 errors.toml 的归类没有真的铺开。
    std::set<ErrorDomain> domains;
    std::set<ErrorKind> kinds;
    for (const auto &entry : AURORA_ERROR_TABLE) {
        // 表内取值必须落在枚举里：to_string 兜底 "unknown" 就是越界的可观测信号。
        AURORA_TEST_CHECK_FALSE(aurora::to_string(entry.domain) == "unknown");
        AURORA_TEST_CHECK_FALSE(aurora::to_string(entry.kind) == "unknown");
        domains.insert(entry.domain);
        kinds.insert(entry.kind);
    }
    AURORA_TEST_CHECK_EQ(domains.size(), std::size_t{29});
    AURORA_TEST_CHECK_EQ(kinds.size(), std::size_t{12});
}

AURORA_TEST_CASE(lookup_helpers_match_table_entry) {
    constexpr auto code = ErrorCode::LayoutNullChild;
    const auto &meta = AURORA_ERROR_TABLE.at(static_cast<std::size_t>(code));
    AURORA_TEST_CHECK(aurora::slug(code) == meta.slug);
    AURORA_TEST_CHECK(aurora::domain(code) == meta.domain && meta.domain == ErrorDomain::Layout);
    AURORA_TEST_CHECK(aurora::kind(code) == meta.kind && meta.kind == ErrorKind::Invalid);
    AURORA_TEST_CHECK(aurora::severity(code) == meta.severity && meta.severity == ErrorSeverity::Warning);
    AURORA_TEST_CHECK_EQ(aurora::retryable(code), meta.retryable);
    AURORA_TEST_CHECK_STREQ(aurora::hint_of(code), meta.hint);
    // 抽查另一条 retryable 记录。
    AURORA_TEST_CHECK_EQ(aurora::retryable(ErrorCode::RuntimeCoroutineException), true);
    // 抽查正交性：同一枚 kind 落在不同域、同一枚域铺开在不同 kind（拆分不是为了换名，是为了两轴各自可查）。
    AURORA_TEST_CHECK(aurora::kind(ErrorCode::NavDepthExceeded) == ErrorKind::Exhausted &&
                      aurora::domain(ErrorCode::NavDepthExceeded) == ErrorDomain::Navigation);
    AURORA_TEST_CHECK(aurora::kind(ErrorCode::StorageIoError) == ErrorKind::IO &&
                      aurora::domain(ErrorCode::StorageIoError) == ErrorDomain::Storage);
    AURORA_TEST_CHECK(aurora::kind(ErrorCode::GeneralInvalidArgument) == ErrorKind::Invalid &&
                      aurora::domain(ErrorCode::GeneralInvalidArgument) == ErrorDomain::General);
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
    static_assert(aurora::domain(ErrorCode::LayoutNullChild) == ErrorDomain::Layout);
    static_assert(aurora::kind(ErrorCode::LayoutNullChild) == ErrorKind::Invalid);
    static_assert(aurora::retryable(ErrorCode::RuntimeAsyncTimeout));
    // 零值不变量：条目按域重排后，ErrorCode{} 仍须是首行 GeneralUnknown——它是 Error / Diagnostic
    // 的默认哨兵（`code_enum == GeneralUnknown` 据此判定「无码」并退化元数据）。
    static_assert(aurora::ErrorCode::GeneralUnknown == aurora::ErrorCode{});
    static_assert(aurora::AURORA_ERROR_TABLE.at(0).code == aurora::ErrorCode::GeneralUnknown);
    static_assert(aurora::slug(aurora::ErrorCode{}) == "general-unknown");
    AURORA_TEST_CHECK(true);
}

}  // namespace aurora::test_cases::utest_error_codes
