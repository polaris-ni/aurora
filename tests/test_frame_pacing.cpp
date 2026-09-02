// 验证帧调度决策纯函数 compute_wait_timeout：
// 活跃帧按帧预算节流、空闲帧睡到定时任务到期或无限等待、后端自带节拍/不限帧率不叠加 sleep。
// 纯逻辑、无平台依赖。
#include "aurora/window/frame_pacing.h"

#include "test_harness.h"

namespace au = aurora;

AURORA_TEST() {
    constexpr double budget = 16.67; // 60fps 帧预算

    // ---- 1. 完全空闲（无脏/无动画/无定时任务）→ 无限等待（-1，纯事件驱动）----
    AURORA_TEST_CHECK(au::compute_wait_timeout(false, false, -1.0, budget, 2.0) < 0.0);

    // ---- 2. 空闲但有定时任务 → 睡到最近到期时刻 ----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(false, false, 250.0, budget, 2.0), 250.0, 1e-4);
    // 已到期（0）→ 立即进入下一帧
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(false, false, 0.0, budget, 2.0), 0.0, 1e-4);

    // ---- 3. 有脏区（活跃帧）→ 剩余预算内节流 ----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(true, false, -1.0, budget, 4.0), budget - 4.0, 1e-4);
    // 有动画同理
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(false, true, -1.0, budget, 4.0), budget - 4.0, 1e-4);

    // ---- 4. 活跃帧预算已超支 → 0（不等待，但不为负）----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(true, false, -1.0, budget, 20.0), 0.0, 1e-4);

    // ---- 5. 活跃帧 + 不限帧率（budget<=0，max_fps=0 旧行为）→ 0 ----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(true, false, -1.0, 0.0, 4.0), 0.0, 1e-4);

    // ---- 6. 活跃帧 + 后端自带节拍（D3D11 vsync）→ 0（避免双重限速）----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(true, false, -1.0, budget, 4.0, true), 0.0, 1e-4);

    // ---- 7. 空闲帧不受后端节拍影响：仍无限等待/睡到定时任务 ----
    AURORA_TEST_CHECK(au::compute_wait_timeout(false, false, -1.0, budget, 4.0, true) < 0.0);
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(false, false, 100.0, budget, 4.0, true), 100.0, 1e-4);

    // ---- 8. 活跃帧优先于定时任务：预算节流生效（不睡到远处的定时任务）----
    AURORA_TEST_CHECK_NEAR(au::compute_wait_timeout(true, false, 500.0, budget, 4.0), budget - 4.0, 1e-4);
}
