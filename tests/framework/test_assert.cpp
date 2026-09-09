#include <cstdio>
#include <memory>
#include <string>
#include <typeinfo>

#include "assertions.h"
#include "test_types.h"

// 反修饰仅服务于诊断文本。`<cxxabi.h>` 只在 GNU 系标准库里；clang-tidy 以自身 target 解析
// GCC 标准库时可能取不到该头，故探测存在后再包含，缺失时退回 typeid 的原始名字。
#if __has_include(<cxxabi.h>) && (defined(__GNUC__) || defined(__clang__))
#include <cxxabi.h>
#define AURORA_TEST_HAVE_CXXABI 1  // NOLINT(*-macro-usage)
#endif

namespace aurora::testing {

auto current_context_slot() -> TestContext*& {
    // 函数内 thread_local：既规避静态初始化顺序问题，也让「无活动用例」的状态可判定。
    static thread_local TestContext* slot = nullptr;
    return slot;
}

auto current_context() -> TestContext* { return current_context_slot(); }

}  // namespace aurora::testing

namespace aurora::testing::detail {

namespace {

/// @brief 静态初始化期 / 用例外断言的失败计数（仅供 runner 启动自检读取）。
auto orphan_failure_count() -> int& {
    static int count = 0;
    return count;
}

/// @brief 用例外断言的兜底报告：无上下文可归属，直接写诊断流并计数。
auto report_orphan(const char* file, int line, const std::string& message) -> void {
    std::fprintf(stderr, "[test] assertion outside any test case at %s:%d: %s\n", file, line, message.c_str());
    ++orphan_failure_count();
}

}  // namespace

auto report(Severity severity, const char* file, int line, std::string message) -> void {
    auto* context = current_context();
    std::string aborted;
    if (context == nullptr) {
        report_orphan(file, line, message);
        aborted = std::move(message);
    } else {
        message += context->trace_text();
        aborted = message;
        context->add_failure(Failure{.file = std::string{file}, .line = line, .message = std::move(message)});
    }
    if (severity == Severity::Fatal) {
        throw CaseAbort{std::move(aborted)};
    }
}

// sink 语义：失败路径按值收下 `reason` 并直接 move 进异常对象，改 const& 反而多一次分配。
// NOLINTNEXTLINE(performance-unnecessary-value-param)
auto skip_case(std::string reason) -> void { throw CaseSkipped{std::move(reason)}; }

auto bool_message(bool satisfied, std::string_view expression) -> std::string {
    if (satisfied) {
        return {};
    }
    return std::string{expression} + " is false";
}

auto expected_bool_message(bool value, bool expected, std::string_view expression) -> std::string {
    if (value == expected) {
        return {};
    }
    return "Value of: " + std::string{expression} + "\n    Actual: " + (value ? "true" : "false") +
           "\n  Expected: " + (expected ? "true" : "false");
}

auto message_message(bool satisfied, std::string_view expression, std::string_view message) -> std::string {
    if (satisfied) {
        return {};
    }
    return std::string{expression} + ": " + std::string{message};
}

auto strings_equal(std::string_view lhs, std::string_view rhs, bool case_sensitive) -> bool {
    if (case_sensitive) {
        return lhs == rhs;
    }
    if (lhs.size() != rhs.size()) {
        return false;
    }
    const auto fold = [](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (fold(lhs[index]) != fold(rhs[index])) {
            return false;
        }
    }
    return true;
}

auto string_message(std::string_view lhs_text, std::string_view rhs_text, std::string_view lhs_actual,
                    std::string_view rhs_actual, bool want_equal, bool case_sensitive) -> std::string {
    const bool equal = strings_equal(lhs_actual, rhs_actual, case_sensitive);
    if (equal == want_equal) {
        return {};
    }
    const std::string op = [&]() -> std::string {
        if (want_equal) {
            return case_sensitive ? std::string{" == "} : std::string{" (case-insensitive) == "};
        }
        return case_sensitive ? std::string{" != "} : std::string{" (case-insensitive) != "};
    }();
    return std::string{lhs_text} + op + std::string{rhs_text} + "\n    Which is: " + quote(lhs_actual) + " vs " +
           quote(rhs_actual);
}

auto exception_text(const std::exception& error) -> std::string {
    const auto& info = typeid(error);
    std::string name = info.name();
#ifdef AURORA_TEST_HAVE_CXXABI
    int status = 0;
    const std::unique_ptr<char, void (*)(void*)> demangled{abi::__cxa_demangle(info.name(), nullptr, nullptr, &status),
                                                           std::free};
    if (demangled != nullptr && status == 0) {
        name = demangled.get();
    }
#endif
    std::string text = "exception " + std::move(name);
    const char* what = error.what();
    if (what != nullptr && *what != '\0') {
        text += " («" + std::string{what} + "»)";
    }
    return text;
}

// sink 语义：有上下文时把 `note` 直接 move 入追踪栈，无上下文即丢弃；改 const& 只会多一次拷贝。
// NOLINTNEXTLINE(performance-unnecessary-value-param)
TraceScope::TraceScope(std::string note) {
    auto* context = current_context();
    if (context == nullptr) {
        return;  // 用例外（如静态初始化期）无上下文可归属：不挂载，析构亦不弹栈。
    }
    context->push_trace(std::move(note));
    attached_ = true;
}

TraceScope::~TraceScope() {
    if (attached_) {
        if (auto* context = current_context(); context != nullptr) {
            context->pop_trace();
        }
    }
}

}  // namespace aurora::testing::detail
