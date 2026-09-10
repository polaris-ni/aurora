/// 测试类型: unit
/// 目标单元: include/aurora/core/aurora_assert.h
/// 测试说明: 覆盖两级断言契约——AURORA_CHECK 常开（通过路径不中断、失败死亡测试、无花括号 if/else
/// 安全组合）、AURORA_ASSERT debug-only（Debug 下求值一次与死亡行为、NDEBUG 下整体裁切条件不求值）

#include "aurora/core/aurora_assert.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_assert {

// ---- AURORA_CHECK：常开（所有构建配置行为一致，对标 Chromium CHECK）----

AURORA_TEST_CASE(check_passing_condition_does_not_abort) {
    // 通过路径：硬检查成立时不得中断进程。
    int guarded = 1;
    AURORA_TEST_CHECK_NO_THROW({ AURORA_CHECK(guarded == 1, "不变量成立"); });
    AURORA_TEST_CHECK_EQ(guarded, 1);
}

AURORA_TEST_CASE(check_failing_condition_aborts_process) {
    // 硬检查无 NDEBUG 裁切：失败 → FATAL 日志 + abort，所有构建生效。
    AURORA_TEST_CHECK_DEATH(AURORA_CHECK(1 == 2, "utest 预期失败的硬不变量"), "");
}

AURORA_TEST_CASE(check_macro_shape_composes_with_unbraced_if_else) {
    // do { } while (0) 包装的经典契约：在无花括号的 if/else 分支中作为单条语句使用，
    // 不会吞掉 else 分支、也不会产生悬空分号问题。
    int taken = 0;
    // clang-format off
    // NOLINTBEGIN
    if (true)
        AURORA_CHECK(true, "then 分支内的检查");
    else
        AURORA_CHECK(false, "不可达分支");
    // NOLINTEND
    // clang-format on
    taken = 1;
    AURORA_TEST_CHECK_EQ(taken, 1);
}

// ---- AURORA_ASSERT：debug-only（NDEBUG 下整体裁切、条件不求值，对标 DCHECK）----

AURORA_TEST_CASE(assert_passing_condition_does_not_abort) {
    // Debug：断言成立不中断；Release：宏裁切为 (void)0，同样不中断。
    constexpr int guarded = 1;
    AURORA_TEST_CHECK_NO_THROW({ AURORA_ASSERT(guarded == 1, "不变量成立"); });
    AURORA_TEST_CHECK_EQ(guarded, 1);  // 兜底使用，避免裁切后 unused-variable
}

AURORA_TEST_CASE(assert_condition_evaluation_follows_build_config) {
    // Debug：条件真实求值一次（副作用可见）；Release：整体裁切、条件不求值。
    int evaluated = 0;
    AURORA_TEST_CHECK_NO_THROW({ AURORA_ASSERT(++evaluated > 0, "条件被求值"); });
#ifndef NDEBUG
    AURORA_TEST_CHECK_EQ(evaluated, 1);
#else
    AURORA_TEST_CHECK_EQ(evaluated, 0);  // NDEBUG 下条件被吞掉，副作用不可见
#endif
}

AURORA_TEST_CASE(assert_macro_shape_composes_with_unbraced_if_else) {
    int taken = 0;
    // clang-format off
    // NOLINTBEGIN
    if (true)
        AURORA_ASSERT(true, "then 分支内的断言");
    else
        AURORA_ASSERT(false, "不可达分支");  // Release 下裁切，else 分支不会触发
    // NOLINTEND
    // clang-format on
    taken = 1;
    AURORA_TEST_CHECK_EQ(taken, 1);
}

AURORA_TEST_CASE(assert_failing_condition_aborts_in_debug_only) {
    // Debug 下验证「断言失败 → 进程异常终止」；Release 下按契约编译掉、死亡行为不适用。
#ifndef NDEBUG
    AURORA_TEST_CHECK_DEATH(AURORA_ASSERT(1 == 2, "utest 预期失败的不变量"), "");
#else
    AURORA_TEST_SKIP("Release（NDEBUG）构建下 AURORA_ASSERT 按契约编译掉，死亡行为不适用");
#endif
}

}  // namespace aurora::test_cases::utest_assert
