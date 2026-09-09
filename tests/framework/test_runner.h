#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "test_types.h"

namespace aurora::testing {

/// @brief 单个用例的执行结果。
struct CaseResult {
    std::string full_name;  ///< `Suite.Case`
    std::string file;  ///< 注册所在源文件
    int line = 0;  ///< 注册所在行
    TestStatus status = TestStatus::Passed;  ///< 执行结果
    std::vector<Failure> failures;  ///< 失败明细（Skipped 时为空）
    std::vector<std::string> notes;  ///< 诊断笔记
    std::string skip_reason;  ///< 跳过原因（仅 Skipped 时有效）
    double elapsed_ms = 0.0;  ///< 墙钟耗时
};

/// @brief 一轮运行的汇总。
struct RunSummary {
    int total = 0;  ///< 执行用例数
    int passed = 0;  ///< 通过数
    int failed = 0;  ///< 失败数
    int skipped = 0;  ///< 跳过数
    double elapsed_ms = 0.0;
};

/// @brief 退出码协议（CI 据此区分「失败」与「超时 / 用法错误」）。
enum class ExitCode {
    AllPassed = 0,  ///< 全部通过（Skipped 不计失败）
    HasFailures = 1,  ///< 至少一个用例失败
    UsageOrNoMatch = 2,  ///< CLI 参数错误，或筛选用例集合为空
    Timeout = 3,  ///< 用例超时（诊断阶段启用，此处预留协议位）
};

namespace detail {

/// @brief 接管当前线程用例上下文，析构时恢复；保证异常路径亦不泄漏上下文。
class ContextGuard {
  public:
    explicit ContextGuard(TestContext& context) : previous_(current_context_slot()) {
        current_context_slot() = &context;
    }
    ContextGuard(const ContextGuard&) = delete;
    auto operator=(const ContextGuard&) -> ContextGuard& = delete;
    ~ContextGuard() { current_context_slot() = previous_; }

  private:
    TestContext* previous_;
};

}  // namespace detail

/// @brief 按套件名与全名子串筛选用例；两者皆空表示全选。
[[nodiscard]] auto select_cases(const std::vector<const TestCase*>& cases, std::string_view suite_filter,
                                std::string_view name_filter) -> std::vector<const TestCase*>;

/// @brief 执行单个用例：隔离异常、接管上下文、计时。
[[nodiscard]] auto run_case(const TestCase& test_case) -> CaseResult;

/// @brief 打印单条用例结果（进度、状态、失败明细、跳过原因；verbose 时附诊断笔记）。
///
/// 执行循环由 `test_main.cpp` 掌握：报告与超时 watchdog 都要能随时读到「已完成的部分结果」，
/// 故框架不提供「整批执行」的黑盒接口。
auto print_case_result(const CaseResult& result, bool verbose) -> void;

/// @brief 汇总执行结果。
[[nodiscard]] auto summarize(const std::vector<CaseResult>& results) -> RunSummary;

/// @brief 由汇总推导退出码。
[[nodiscard]] auto exit_code_for(const RunSummary& summary) -> int;

/// @brief 打印汇总块。
auto print_summary(const RunSummary& summary) -> void;

}  // namespace aurora::testing
