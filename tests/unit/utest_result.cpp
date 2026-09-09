/// 测试类型: unit
/// 目标单元: include/aurora/core/result.h
/// 测试说明: Result<T>/Result<void> 的构造与不变量、值取回与 unwrap、错误路径，以及 make_error 三重载的表驱动元数据/模板渲染与 Error::to_json 形态

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "aurora/core/result.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_result {

namespace m = aurora::testing::matchers;

AURORA_TEST_CASE(result_value_state_and_bool_conversion) {
    Result<int> r{42};
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK(r);  // explicit operator bool：成功为 true
    AURORA_TEST_CHECK_EQ(r.value(), 42);

    // 非 const value() 可写回。
    r.value() = 43;
    AURORA_TEST_CHECK_EQ(r.value(), 43);

    // const 访问返回只读引用。
    const Result<int>& view = r;
    AURORA_TEST_CHECK_EQ(view.value(), 43);
}

AURORA_TEST_CASE(result_error_state_and_access) {
    Error err = make_error(ErrorCode::GeneralInvalidArgument);
    err.message = "bad input";
    const Result<int> r{err};
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(r));
    AURORA_TEST_CHECK_STREQ(r.error().message, "bad input");
    AURORA_TEST_CHECK(r.error().code_enum == ErrorCode::GeneralInvalidArgument);
}

AURORA_TEST_CASE(result_unwrap_value_or_throws_runtime_error) {
    // 成功路径：解包返回值。
    AURORA_TEST_CHECK_EQ(Result<int>{7}.unwrap(), 7);

    // 失败路径：抛 std::runtime_error，消息为 Error::message。
    Result<int> bad{make_error(ErrorCode::NavDepthExceeded, ErrorParams{{"max", "4"}}, std::string{"unwrap boom"})};
    AURORA_TEST_CHECK_THROW(static_cast<void>(bad.unwrap()), std::runtime_error);

    bool threw = false;
    try {
        (void)bad.unwrap();
    } catch (const std::runtime_error& e) {
        threw = true;
        AURORA_TEST_CHECK_EQ(e.what(), bad.error().message);
    }
    AURORA_TEST_CHECK_TRUE(threw);
}

AURORA_TEST_CASE(result_void_success_and_error_paths) {
    // Result<void> 成功态：默认构造即成功，无 value() 可取。
    const Result<void> ok{};
    AURORA_TEST_CHECK_TRUE(ok.ok());
    AURORA_TEST_CHECK(ok);

    // 失败态：从 Error 隐式构造（统一失败路径）。
    const auto fail = []() -> Result<void> {
        return make_error(ErrorCode::IOParseFailed, ErrorParams{{"detail", "bad json"}});
    };
    const Result<void> bad = fail();
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK(bad.error().code_enum == ErrorCode::IOParseFailed);
    AURORA_TEST_CHECK_THAT(bad.error().message, m::has_substr("bad json"));
}

AURORA_TEST_CASE(make_error_renders_template_and_fills_table_metadata) {
    // 主入口：用 errors.toml 的 message 模板渲染 {placeholder}，元数据全量来自表。
    const auto e = make_error(ErrorCode::NavDepthExceeded, ErrorParams{{"max", "8"}});
    AURORA_TEST_CHECK_STREQ(e.message, "Navigation stack depth exceeded the limit (default 8)");
    AURORA_TEST_CHECK_STREQ(e.code, "nav-depth-exceeded");
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::NavDepthExceeded);
    AURORA_TEST_CHECK(e.severity == ErrorSeverity::Error);
    AURORA_TEST_CHECK(e.category == ErrorCategory::Navigation);
    AURORA_TEST_CHECK_EQ(e.auto_fixable, false);
    AURORA_TEST_CHECK_EQ(e.retryable, false);
    AURORA_TEST_CHECK_STREQ(e.fix_category, "layout_conflict");
    AURORA_TEST_CHECK_STREQ(e.hint, std::string{aurora::hint_of(ErrorCode::NavDepthExceeded)});
}

AURORA_TEST_CASE(make_error_hint_override_and_custom_message) {
    // hint 覆盖：非空 hint 优先于表内默认 hint。
    const auto overridden = make_error(ErrorCode::WidgetInvalidProp, ErrorParams{{"prop", "color"}}, std::string{"custom hint"});
    AURORA_TEST_CHECK_STREQ(overridden.message, "Invalid property value: 'color'");
    AURORA_TEST_CHECK_STREQ(overridden.hint, "custom hint");

    // 自定义 message 重载：覆盖模板，其余元数据仍来自表。
    const auto custom = make_error(ErrorCode::JsonParseError, std::string{"my own message"});
    AURORA_TEST_CHECK_STREQ(custom.message, "my own message");
    AURORA_TEST_CHECK_STREQ(custom.hint, std::string{aurora::hint_of(ErrorCode::JsonParseError)});
    AURORA_TEST_CHECK_STREQ(custom.code, "json-parse-error");

    // 自定义 message 时 params 不参与渲染（调用方已提供最终文案）。
    const auto ignored = make_error(ErrorCode::JsonParseError, std::string{"raw"}, ErrorParams{{"x", "y"}});
    AURORA_TEST_CHECK_STREQ(ignored.message, "raw");
}

AURORA_TEST_CASE(make_error_legacy_overload_sets_suggestion_docs_where) {
    // 向后兼容重载：suggestion/docs/where 由调用方提供，slug/severity/category 仍来自表。
    const auto e = make_error(ErrorCode::IOFileNotFound, std::string{"cfg.json missing"}, std::string{"Check the path"},
                              std::string{"docs/io.md"}, std::string{"preferences.cpp:42"});
    AURORA_TEST_CHECK_STREQ(e.message, "cfg.json missing");
    AURORA_TEST_CHECK_STREQ(e.suggestion, "Check the path");
    AURORA_TEST_CHECK_STREQ(e.docs, "docs/io.md");
    AURORA_TEST_CHECK_STREQ(e.where, "preferences.cpp:42");
    AURORA_TEST_CHECK_STREQ(e.code, "io-file-not-found");
    AURORA_TEST_CHECK(e.severity == ErrorSeverity::Error);
    AURORA_TEST_CHECK(e.category == ErrorCategory::Io);
}

AURORA_TEST_CASE(make_error_from_table_fills_frozen_slug_metadata) {
    // make_error_from_table：空 hint 退化取表内默认 hint；slug 为冻结对外标识。
    const auto e = make_error_from_table(ErrorCode::RuntimeAsyncTimeout, "timeout message", {});
    AURORA_TEST_CHECK_STREQ(e.code, "async-timeout");  // 冻结 slug 与 C++ 标识符解耦
    AURORA_TEST_CHECK_STREQ(e.message, "timeout message");
    AURORA_TEST_CHECK_EQ(e.retryable, true);
    AURORA_TEST_CHECK_STREQ(e.fix_category, "timeout");
    AURORA_TEST_CHECK_STREQ(e.hint, std::string{aurora::hint_of(ErrorCode::RuntimeAsyncTimeout)});
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::RuntimeAsyncTimeout);
}

AURORA_TEST_CASE(error_default_construction_uses_neutral_metadata) {
    const Error d{};
    AURORA_TEST_CHECK(d.code.empty());
    AURORA_TEST_CHECK(d.message.empty());
    AURORA_TEST_CHECK(d.code_enum == ErrorCode::GeneralUnknown);
    AURORA_TEST_CHECK(d.severity == ErrorSeverity::Error);
    AURORA_TEST_CHECK(d.category == ErrorCategory::General);
    AURORA_TEST_CHECK_EQ(d.auto_fixable, false);
    AURORA_TEST_CHECK_EQ(d.retryable, false);
}

AURORA_TEST_CASE(error_to_json_shape_and_escaping) {
    auto e = make_error(ErrorCode::IOFileNotFound, std::string{R"(missing "cfg.json)"}, std::string{"check path"});
    e.where = "a\\b";
    const auto json = e.to_json();
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("code":"io-file-not-found")"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("message":"missing \"cfg.json")"));  // 引号被转义
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("suggestion":"check path")"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("where":"a\\b")"));  // 反斜杠被转义
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("code_enum":"IOFileNotFound")"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("severity":"error")"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("category":"io")"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("auto_fixable":false)"));
    AURORA_TEST_CHECK_THAT(json, m::has_substr(R"("retryable":false)"));
}

}  // namespace aurora::test_cases::utest_result
