#include "test_runner.h"

#include <chrono>
#include <cstdio>
#include <string>

#include "assertions.h"
#include "death_test.h"

namespace aurora::testing {

namespace {

/// @brief 状态标签（定宽，与 gtest 输出对齐以便人眼扫描）。
[[nodiscard]] auto status_label(TestStatus status) -> const char* {
    switch (status) {
        case TestStatus::Passed:
            return "       OK ";
        case TestStatus::Failed:
            return "  FAILED  ";
        case TestStatus::Skipped:
            return "  SKIPPED ";
    }
    return "  UNKNOWN ";
}

[[nodiscard]] auto contains(std::string_view haystack, std::string_view needle) -> bool {
    return needle.empty() || haystack.find(needle) != std::string_view::npos;
}

}  // namespace

auto select_cases(const std::vector<const TestCase*>& cases, std::string_view suite_filter,
                  std::string_view name_filter) -> std::vector<const TestCase*> {
    std::vector<const TestCase*> selected;
    selected.reserve(cases.size());
    for (const auto* test_case : cases) {
        if (!suite_filter.empty() && test_case->suite != suite_filter) {
            continue;
        }
        // 先比套件再拼全名：full_name() 会分配，避免对不相关用例白做一次拼接。
        if (!name_filter.empty() && !contains(test_case->full_name(), name_filter)) {
            continue;
        }
        selected.push_back(test_case);
    }
    return selected;
}

auto run_case(const TestCase& test_case) -> CaseResult {
    CaseResult result;
    result.full_name = test_case.full_name();
    result.file = test_case.file;
    result.line = test_case.line;

    const auto start = std::chrono::steady_clock::now();
    {
        TestContext context;
        // 用例全名先登记：死亡测试需要以「同一用例」为单位重跑子进程。
        context.set_subject(result.full_name);
        const detail::ContextGuard guard{context};
        if (detail::death_child_mode()) {
            // 死亡测试子进程不隔离异常：目标语句抛出即逃到进程外触发 terminate，
            // 那才是「致死」判据；在这里被捕获就只剩一次普通失败。
            test_case.run_body();
        } else {
            try {
                test_case.run_body();  // 普通用例走 body，参数化用例走 param_body + 取值序号
            } catch (const CaseSkipped& skipped) {
                result.status = TestStatus::Skipped;
                result.skip_reason = skipped.reason();
            } catch (const CaseAbort&) {
                // 失败明细已由断言内核（report + Severity::Fatal）登记进 context，此处仅定状态。
                result.status = TestStatus::Failed;
            } catch (const std::exception& ex) {
                context.add_failure(Failure{.file = std::string{test_case.file},
                                            .line = test_case.line,
                                            .message = std::string{"unexpected exception: "} + ex.what()});
                result.status = TestStatus::Failed;
            } catch (...) {
                context.add_failure(Failure{.file = std::string{test_case.file},
                                            .line = test_case.line,
                                            .message = "unexpected non-standard exception"});
                result.status = TestStatus::Failed;
            }
        }
        result.failures = context.failures();
        result.notes = context.notes();
        if (result.status != TestStatus::Skipped && context.failed()) {
            result.status = TestStatus::Failed;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
}

auto print_case_result(const CaseResult& result, bool verbose) -> void {
    std::printf("[%s] %s (%.2f ms)\n", status_label(result.status), result.full_name.c_str(), result.elapsed_ms);
    for (const auto& failure : result.failures) {
        std::printf("  %s:%d: %s\n", failure.file.c_str(), failure.line, failure.message.c_str());
    }
    if (result.status == TestStatus::Skipped && !result.skip_reason.empty()) {
        std::printf("  skip reason: %s\n", result.skip_reason.c_str());
    }
    if (verbose) {
        for (const auto& note : result.notes) {
            std::printf("  note: %s\n", note.c_str());
        }
    }
}

auto summarize(const std::vector<CaseResult>& results) -> RunSummary {
    RunSummary summary;
    summary.total = static_cast<int>(results.size());
    for (const auto& result : results) {
        switch (result.status) {
            case TestStatus::Passed:
                ++summary.passed;
                break;
            case TestStatus::Failed:
                ++summary.failed;
                break;
            case TestStatus::Skipped:
                ++summary.skipped;
                break;
        }
        summary.elapsed_ms += result.elapsed_ms;
    }
    return summary;
}

auto exit_code_for(const RunSummary& summary) -> int {
    return summary.failed > 0 ? static_cast<int>(ExitCode::HasFailures) : static_cast<int>(ExitCode::AllPassed);
}

auto print_summary(const RunSummary& summary) -> void {
    std::printf("[==========] %d test case(s) ran (%.2f ms total)\n", summary.total, summary.elapsed_ms);
    std::printf("[  PASSED  ] %d case(s)\n", summary.passed);
    if (summary.failed > 0) {
        std::printf("[  FAILED  ] %d case(s)\n", summary.failed);
    }
    if (summary.skipped > 0) {
        std::printf("[  SKIPPED ] %d case(s)\n", summary.skipped);
    }
}

}  // namespace aurora::testing
