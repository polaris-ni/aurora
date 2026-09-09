/// 测试类型: unit
/// 目标单元: include/aurora/app/scheduler.h
/// 测试说明: 覆盖 Scheduler 纯逻辑调度——一次性任务到期触发与自动移除、cancel/clear 取消、
/// 周期任务重复触发、同帧多任务齐触发、next_deadline_ms 语义、回调内重注册安全性、
/// TimerHandle 值语义与线程局部 current 实例（手动 tick 虚拟时钟，不依赖真实时间等待）

#include <chrono>

#include "aurora/app/scheduler.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scheduler {

namespace {

using std::chrono::milliseconds;

}  // namespace

AURORA_TEST_CASE(default_timer_handle_is_inactive) {
    // 默认句柄无条目：active 为假，cancel 为安全 no-op。
    const TimerHandle handle;
    AURORA_TEST_CHECK_FALSE(handle.active());
    AURORA_TEST_CHECK_NO_THROW(handle.cancel());
    AURORA_TEST_CHECK_FALSE(handle.active());
}

AURORA_TEST_CASE(timeout_fires_once_after_due) {
    Scheduler sched;
    int fired = 0;
    const TimerHandle handle = sched.set_timeout(milliseconds{100}, [&fired]() -> void { ++fired; });
    AURORA_TEST_CHECK_TRUE(handle.active());

    // 未到期不触发。
    sched.tick(0.05);
    AURORA_TEST_CHECK_EQ(fired, 0);
    AURORA_TEST_CHECK_TRUE(handle.active());

    // 累计到达 100ms 恰好触发一次；句柄随之失效（一次性条目已剪除）。
    sched.tick(0.05);
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_FALSE(handle.active());

    // 之后再 tick 不重复触发。
    sched.tick(0.10);
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(cancel_prevents_timeout_firing) {
    Scheduler sched;
    int fired = 0;
    const TimerHandle handle = sched.set_timeout(milliseconds{50}, [&fired]() -> void { ++fired; });

    handle.cancel();
    AURORA_TEST_CHECK_FALSE(handle.active());

    sched.tick(0.20);
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(interval_fires_repeatedly_until_cancel) {
    Scheduler sched;
    int fired = 0;
    const TimerHandle handle = sched.set_interval(milliseconds{50}, [&fired]() -> void { ++fired; });

    // 4 次 40ms（累计 160ms）：50/100/150 三个截止点各触发一次。
    for (int i = 0; i < 4; ++i) {
        sched.tick(0.04);
    }
    AURORA_TEST_CHECK_EQ(fired, 3);
    AURORA_TEST_CHECK_TRUE(handle.active());

    // 取消后不再触发。
    handle.cancel();
    AURORA_TEST_CHECK_FALSE(handle.active());
    sched.tick(0.04);
    sched.tick(0.04);
    AURORA_TEST_CHECK_EQ(fired, 3);
}

AURORA_TEST_CASE(all_due_tasks_fire_in_single_tick) {
    Scheduler sched;
    int fired_a = 0;
    int fired_b = 0;
    (void)sched.set_timeout(milliseconds{100}, [&fired_a]() -> void { ++fired_a; });
    (void)sched.set_timeout(milliseconds{50}, [&fired_b]() -> void { ++fired_b; });

    // 一大步跨过两个截止点：同帧内到期任务全部触发，且各自只触发一次。
    sched.tick(0.20);
    AURORA_TEST_CHECK_EQ(fired_a, 1);
    AURORA_TEST_CHECK_EQ(fired_b, 1);
    AURORA_TEST_CHECK_LT(sched.next_deadline_ms(), 0.0);  // 一次性条目均已剪除
}

AURORA_TEST_CASE(clear_cancels_all_pending_tasks) {
    Scheduler sched;
    int fired = 0;
    const TimerHandle timeout = sched.set_timeout(milliseconds{50}, [&fired]() -> void { ++fired; });
    const TimerHandle interval = sched.set_interval(milliseconds{50}, [&fired]() -> void { ++fired; });

    sched.clear();
    AURORA_TEST_CHECK_FALSE(timeout.active());
    AURORA_TEST_CHECK_FALSE(interval.active());

    sched.tick(1.0);
    AURORA_TEST_CHECK_EQ(fired, 0);
    AURORA_TEST_CHECK_LT(sched.next_deadline_ms(), 0.0);
}

AURORA_TEST_CASE(next_deadline_ms_tracks_soonest_task) {
    Scheduler sched;

    // 无任务：-1。
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), -1.0, 1e-4);

    const TimerHandle far_task = sched.set_timeout(milliseconds{100}, []() -> void {});
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), 100.0, 1e-4);

    // 推进 50ms 后剩余 50ms。
    sched.tick(0.05);
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), 50.0, 1e-4);

    // 更近的任务成为最近截止。
    const TimerHandle near_task = sched.set_timeout(milliseconds{10}, []() -> void {});
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), 10.0, 1e-4);

    // 取消更近任务后回到 50ms；已取消条目不参与计算。
    near_task.cancel();
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), 50.0, 1e-4);

    far_task.cancel();
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), -1.0, 1e-4);
}

AURORA_TEST_CASE(next_deadline_ms_negative_without_pending_tasks) {
    Scheduler sched;
    int fired = 0;
    (void)sched.set_timeout(milliseconds{50}, [&fired]() -> void { ++fired; });

    sched.tick(0.10);  // 已触发并剪除
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_NEAR(sched.next_deadline_ms(), -1.0, 1e-4);
}

AURORA_TEST_CASE(callback_may_register_new_tasks_during_fire) {
    // 回调内重注册不失效（先收集到期项再统一触发）：周期回调里再排一次性任务。
    Scheduler sched;
    int interval_fired = 0;
    int inner_fired = 0;

    (void)sched.set_interval(milliseconds{100}, [&]() -> void {
        ++interval_fired;
        (void)sched.set_timeout(milliseconds{10}, [&inner_fired]() -> void { ++inner_fired; });
    });

    // tick1（150ms）：周期触发第 1 次，登记 inner#1（截止 160ms）。
    sched.tick(0.15);
    AURORA_TEST_CHECK_EQ(interval_fired, 1);
    AURORA_TEST_CHECK_EQ(inner_fired, 0);

    // tick2（+50ms = 200ms）：周期第 2 次（登记 inner#2，截止 210ms），
    // inner#1（160ms）同帧到期触发。
    sched.tick(0.05);
    AURORA_TEST_CHECK_EQ(interval_fired, 2);
    AURORA_TEST_CHECK_EQ(inner_fired, 1);

    // inner#2 在下一次 tick 触发。
    sched.tick(0.05);
    AURORA_TEST_CHECK_EQ(inner_fired, 2);
}

AURORA_TEST_CASE(thread_local_current_instance_roundtrip) {
    // 用例前后复位，避免污染同进程内其它用例。
    Scheduler::set_current(nullptr);
    AURORA_TEST_CHECK_EQ(Scheduler::current(), static_cast<Scheduler*>(nullptr));

    Scheduler sched;
    Scheduler::set_current(&sched);
    AURORA_TEST_CHECK_EQ(Scheduler::current(), &sched);

    Scheduler::set_current(nullptr);
    AURORA_TEST_CHECK_EQ(Scheduler::current(), static_cast<Scheduler*>(nullptr));
}

}  // namespace aurora::test_cases::utest_scheduler
