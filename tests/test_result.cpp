#include <cstdio>
#include <stdexcept>
#include <string>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

#include "test_harness.h"

using aurora::Error;
using aurora::ErrorCode;
using aurora::Result;

static void test_ok() {
    Result r(42);
    AURORA_TEST_CHECK_MSG(r.ok(), "Result: ok() true for value");
    AURORA_TEST_CHECK_MSG(static_cast<bool>(r), "Result: bool conversion true");
    AURORA_TEST_CHECK_MSG(r.value() == 42, "Result: value()==42");
}

static void test_error() {
    Error e = make_error(ErrorCode::GeneralUnknown, "it failed");
    Result<int> r(e);
    AURORA_TEST_CHECK_MSG(!r.ok(), "Result: ok() false for error");
    AURORA_TEST_CHECK_MSG(!(static_cast<bool>(r)), "Result: bool conversion false");
    AURORA_TEST_CHECK_MSG(r.error().code == "general-unknown", "Result: error code is slug");
    AURORA_TEST_CHECK_MSG(r.error().code_enum == ErrorCode::GeneralUnknown, "Result: error code_enum");
    AURORA_TEST_CHECK_MSG(r.error().message == "it failed", "Result: error message");

    bool threw = false;
    try {
        static_cast<void>(r.unwrap());
    } catch (const std::runtime_error &) {
        threw = true;
    }
    AURORA_TEST_CHECK_MSG(threw, "Result: unwrap() throws on error");
}

static void test_make_error_overloads() {
    // (enum, params)：message 由表模板渲染，元数据来自表
    auto e1 = make_error(ErrorCode::LayoutDepthExceeded, { { "max", "10" } });
    AURORA_TEST_CHECK_MSG(e1.code == "layout-depth-exceeded", "make_error(enum,params): slug");
    AURORA_TEST_CHECK_MSG(e1.message == "Layout tree depth exceeded the limit (default 10)",
                          "make_error(enum,params): templated message");
    AURORA_TEST_CHECK_MSG(e1.auto_fixable, "make_error(enum,params): auto_fixable from table");

    // (enum, message)：自定义 message 覆盖表模板
    auto e2 = make_error(ErrorCode::GeneralUnknown, "custom");
    AURORA_TEST_CHECK_MSG(e2.code == "general-unknown" && e2.message == "custom", "make_error(enum,message)");

    // (enum, message, params, hint)：hint 覆盖表默认
    auto e3 = make_error(ErrorCode::NavDepthExceeded, "custom msg", { { "max", "5" } }, "my hint");
    AURORA_TEST_CHECK_MSG(e3.hint == "my hint" && e3.message == "custom msg", "make_error(enum,message,params,hint)");

    // (enum, message, suggestion, docs, where)：向后兼容，元数据仍来自表
    auto e4 = make_error(ErrorCode::ValidationFailed, "msg", "fix", "docs#x", "file:1");
    AURORA_TEST_CHECK_MSG(e4.suggestion == "fix" && e4.docs == "docs#x" && e4.where == "file:1",
                          "make_error(enum,message,suggestion,docs,where)");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== result ===\n");
    test_ok();
    test_error();
    test_make_error_overloads();
}
