// test_timers.cpp — 定时任务模块 1:1 测试：Scheduler 单元 + Timer 控件响应式与卸载取消。
// ── API 覆盖映射 ─────────────────────────────
// app/scheduler.h（Scheduler 定时任务调度）。

#include <chrono>
#include <memory>

#include "aurora/aurora.h"

#include "test_harness.h"

using std::chrono_literals::operator""s;
using au::Scheduler;
using au::TimerHandle;

namespace {

void test_scheduler_timeout() {
    Scheduler s;
    int fired = 0;
    const TimerHandle h = s.set_timeout(1s, [&]() -> void { ++fired; });
    AURORA_TEST_CHECK(h.active());
    s.tick(0.5); // 未到期
    AURORA_TEST_CHECK(fired == 0);
    AURORA_TEST_CHECK(h.active());
    s.tick(0.6); // 累计 1.1s，到期触发一次
    AURORA_TEST_CHECK(fired == 1);
    AURORA_TEST_CHECK(!h.active());
    s.tick(1.0); // 一次性已自动移除
    AURORA_TEST_CHECK(fired == 1);
}

void test_scheduler_interval() {
    Scheduler s;
    int fired = 0;
    const TimerHandle h = s.set_interval(1s, [&]() -> void { ++fired; });
    s.tick(0.9);
    AURORA_TEST_CHECK(fired == 0);
    s.tick(0.2); // 1.1s → 1
    AURORA_TEST_CHECK(fired == 1);
    s.tick(1.0); // 2.1s → 2
    AURORA_TEST_CHECK(fired == 2);
    s.tick(1.0); // 3.1s → 3
    AURORA_TEST_CHECK(fired == 3);
    h.cancel();
    AURORA_TEST_CHECK(!h.active());
    s.tick(2.0); // 已取消，不再触发
    AURORA_TEST_CHECK(fired == 3);
}

void test_scheduler_cancel_before_fire() {
    Scheduler s;
    int fired = 0;
    const TimerHandle h = s.set_timeout(1s, [&]() -> void { ++fired; });
    h.cancel();
    AURORA_TEST_CHECK(!h.active());
    s.tick(2.0);
    AURORA_TEST_CHECK(fired == 0);
}

void test_scheduler_clear() {
    Scheduler s;
    int a = 0;
    int b = 0;
    s.set_timeout(1s, [&]() -> void { ++a; });
    s.set_interval(1s, [&]() -> void { ++b; });
    s.tick(0.5);
    s.clear(); // 取消全部（含周期）
    s.tick(2.0);
    AURORA_TEST_CHECK(a == 0);
    AURORA_TEST_CHECK(b == 0);
}

void test_scheduler_two_independent_handles() {
    Scheduler s;
    int a = 0;
    int b = 0;
    const TimerHandle ha = s.set_interval(1s, [&]() -> void { ++a; });
    TimerHandle hb = s.set_interval(1s, [&]() -> void { ++b; });
    s.tick(1.1);
    AURORA_TEST_CHECK(a == 1 && b == 1);
    ha.cancel(); // 仅取消 a
    s.tick(1.0);
    AURORA_TEST_CHECK(a == 1);
    AURORA_TEST_CHECK(b == 2);
}

void test_timer_reactive_and_on_tick() {
    Scheduler s;
    Scheduler::set_current(&s); // 模拟运行中的 App
    int on_tick_count = 0;
    auto timer = std::make_unique<au::Timer>(
        1s, [](const au::SignalView<int> &) -> au::Node { return au::Node{ au::Text{ "x" } }; },
        [&](int n) -> void { on_tick_count = n; });
    constexpr au::BuildContext ctx;
    timer->mount(ctx);
    AURORA_TEST_CHECK(timer->ticks().get() == 0);

    s.tick(0.5);
    AURORA_TEST_CHECK(on_tick_count == 0);
    AURORA_TEST_CHECK(timer->ticks().get() == 0);

    s.tick(0.6); // 1.1s → 第 1 次 tick
    AURORA_TEST_CHECK(on_tick_count == 1);
    AURORA_TEST_CHECK(timer->ticks().get() == 1);

    s.tick(1.0); // 2.1s → 第 2 次
    AURORA_TEST_CHECK(on_tick_count == 2);
    AURORA_TEST_CHECK(timer->ticks().get() == 2);

    // 卸载（析构）应取消句柄，后续 tick 不再触发
    timer.reset();
    s.tick(2.0);
    AURORA_TEST_CHECK(on_tick_count == 2);
    Scheduler::set_current(nullptr);
}

void test_timer_degrade_without_scheduler() {
    Scheduler::set_current(nullptr); // 无运行中 App
    const auto timer = std::make_unique<au::Timer>(
        1s, [](const au::SignalView<int> &) -> au::Node { return au::Node{ au::Text{ "x" } }; });
    constexpr au::BuildContext ctx;
    timer->mount(ctx); // 不应崩溃
    AURORA_TEST_CHECK(timer->ticks().get() == 0);

    Scheduler s;
    s.tick(5.0); // 未注册，不应触发
    AURORA_TEST_CHECK(timer->ticks().get() == 0);
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_timers ===\n");
    test_scheduler_timeout();
    test_scheduler_interval();
    test_scheduler_cancel_before_fire();
    test_scheduler_clear();
    test_scheduler_two_independent_handles();
    test_timer_reactive_and_on_tick();
    test_timer_degrade_without_scheduler();
}
