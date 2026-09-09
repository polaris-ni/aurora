/// 测试类型: unit
/// 目标单元: include/aurora/commands.h
/// 测试说明: 覆盖命令式逃生舱 run_raw——空函数安全、回调执行、nullptr 不崩溃

#include <functional>

#include "aurora/commands.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_commands {

AURORA_TEST_CASE(run_raw_with_null_function_is_safe) {
    // nullptr 回调：no-op，不崩溃。
    AURORA_TEST_CHECK_NO_THROW(aurora::commands::run_raw(nullptr));
    const std::function<void()> empty;
    AURORA_TEST_CHECK_NO_THROW(aurora::commands::run_raw(empty));
}

AURORA_TEST_CASE(run_raw_executes_callback) {
    int calls = 0;
    aurora::commands::run_raw([&calls] { ++calls; });
    AURORA_TEST_CHECK_EQ(calls, 1);
    // 重复调用按次执行。
    aurora::commands::run_raw([&calls] { calls += 2; });
    AURORA_TEST_CHECK_EQ(calls, 3);
}

AURORA_TEST_CASE(run_raw_is_inline_escape_hatch) {
    // 逃生舱语义：函数体内可执行任意命令式操作（如修改外部状态）。
    int value = 10;
    aurora::commands::run_raw([&value] { value = 42; });
    AURORA_TEST_CHECK_EQ(value, 42);
}

}  // namespace aurora::test_cases::utest_commands
