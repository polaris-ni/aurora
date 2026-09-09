/// 测试类型: unit
/// 目标单元: include/aurora/window/frame_pacing.h
/// 测试说明: compute_wait_timeout 帧调度纯逻辑全量分支——活跃帧预算节流/钳零/不限帧率、
/// 后端自带节拍跳过 CPU 节流、空闲帧睡到定时任务/无限等待、纯函数一致性

#include "aurora/window/frame_pacing.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_frame_pacing {

AURORA_TEST_CASE(active_frame_throttles_to_remaining_budget) {
    // 有脏区：睡到下一帧起点 = 帧预算 - 已耗时。
    const double dirty = compute_wait_timeout(true, false, -1.0, 100.0, 25.0);
    AURORA_TEST_CHECK_NEAR(dirty, 75.0, 1e-4F);
    // 动画与脏区走同一条活跃帧路径。
    const double anim = compute_wait_timeout(false, true, -1.0, 16.0, 4.0);
    AURORA_TEST_CHECK_NEAR(anim, 12.0, 1e-4F);
    // 活跃帧忽略定时任务到期时间（活跃帧的节流优先于定时唤醒）。
    const double ignores_deadline = compute_wait_timeout(true, false, 30.0, 100.0, 10.0);
    AURORA_TEST_CHECK_NEAR(ignores_deadline, 90.0, 1e-4F);
}

AURORA_TEST_CASE(active_frame_clamps_elapsed_over_budget) {
    // 已耗时超过预算：剩余为负，钳到 0（立即进入下一帧，不睡负值）。
    const double r = compute_wait_timeout(true, false, -1.0, 100.0, 250.0);
    AURORA_TEST_CHECK_NEAR(r, 0.0, 1e-4F);
    AURORA_TEST_CHECK_GE(r, 0.0);
}

AURORA_TEST_CASE(active_frame_unlimited_budget_returns_zero) {
    // 契约：帧预算 <= 0 表示不限帧率（max_fps=0），活跃帧立即进入下一帧。
    AURORA_TEST_CHECK_NEAR(compute_wait_timeout(true, false, -1.0, 0.0, 10.0), 0.0, 1e-4F);
    AURORA_TEST_CHECK_NEAR(compute_wait_timeout(false, true, 42.0, -5.0, 0.0), 0.0, 1e-4F);
}

AURORA_TEST_CASE(active_frame_backend_paced_returns_zero) {
    // 契约：后端自带帧节拍（如 D3D11 vsync）时 CPU 端跳过 sleep，避免双重限速。
    const double paced = compute_wait_timeout(true, true, 999.0, 100.0, 0.0, true);
    AURORA_TEST_CHECK_NEAR(paced, 0.0, 1e-4F);
    // 对照：同一输入但 backend_paced=false 仍走预算节流。
    const double unpaced = compute_wait_timeout(true, true, 999.0, 100.0, 0.0, false);
    AURORA_TEST_CHECK_NEAR(unpaced, 100.0, 1e-4F);
}

AURORA_TEST_CASE(idle_frame_without_deadline_waits_forever) {
    // 空闲帧（无脏区/动画）且无定时任务（deadline < 0）：无限等待，idle 零 CPU。
    AURORA_TEST_CHECK_NEAR(compute_wait_timeout(false, false, -1.0, 100.0, 0.0), -1.0, 1e-4F);
    AURORA_TEST_CHECK_NEAR(compute_wait_timeout(false, false, -1000.0, 16.0, 5.0), -1.0, 1e-4F);
}

AURORA_TEST_CASE(idle_frame_sleeps_until_deadline) {
    // 空闲帧有定时任务：睡到最近到期时刻（正 deadline 原样透传）。
    AURORA_TEST_CHECK_NEAR(compute_wait_timeout(false, false, 250.0, 16.0, 0.0), 250.0, 1e-4F);
    // 到期时刻已到（0）：不等待。
    const double due = compute_wait_timeout(false, false, 0.0, 16.0, 0.0);
    AURORA_TEST_CHECK_NEAR(due, 0.0, 1e-4F);
    AURORA_TEST_CHECK_GE(due, 0.0);
}

AURORA_TEST_CASE(frame_pacing_is_pure_and_consistent) {
    // 纯函数（无状态、无副作用）：同输入必得同输出。
    const double first = compute_wait_timeout(true, false, 12.0, 50.0, 20.0);
    const double second = compute_wait_timeout(true, false, 12.0, 50.0, 20.0);
    AURORA_TEST_CHECK_NEAR(first, 30.0, 1e-4F);
    AURORA_TEST_CHECK_NEAR(first, second, 1e-4F);
}

}  // namespace aurora::test_cases::utest_frame_pacing
