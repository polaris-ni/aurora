/// 测试类型: unit
/// 目标单元: include/aurora/state/async.h
/// 测试说明: Task<T> 的 then 值投递、Result 透传与异常捕获、cancel 丢弃结果与取消观察者的四条契约
/// （首次取消触发一次、幂等、后登记只留最后一枚、登记时已取消即当场触发）、观察者在锁外执行、
/// 落定与超时都不触发观察者、with_timeout
/// 超时投递与提前完成不误报、结果就绪后注册回调的补投、进程级唯一队列的投递路由，`blocking_wait`
/// 的同步取结果（打烊取消的是投递不是结果），发起侧三分闸门的硬失败一侧（非打烊无值班发起经
/// 注入的失败处理器显形、不 terminate），以及
/// deferred 构建的登记式超时看守（到期表语义：只触发过期项、唤醒期限取最早且过期钳 0、
/// delivered 后改道不二次回调）。全部经 promise/future 或原子量有界等待，断言只在用例线程执行

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/main_queue.h"
#include "aurora/core/platform.h"  // AURORA_CAP_THREADS（deferred 分支的编译期判据）
#include "aurora/core/strict_mode.h"  // 发起侧硬失败面的注入点（set_strict_failure_handler）
#include "aurora/state/async.h"
#include "framework/aurora_test.h"
#include "framework/main_queue_pump.h"  // wait_until / DutyOnly / DutyPump 共享替身

namespace aurora::test_cases::utest_async {

namespace m = aurora::testing::matchers;

using aurora::test::mq::DutyOnly;
using aurora::test::mq::DutyPump;
using aurora::test::mq::wait_until;
using aurora::testing::require_ref;

namespace {

/// @brief 用例退出时清空全局看守表：条目只在到期时被摘除，未到期项若跨用例残留，
///        后续任何帧尾扫描都会打到本用例的栈上对象。以「远超期限的 now」扫一趟即全清。
struct TimeoutGuardsDrain {
    TimeoutGuardsDrain() = default;
    TimeoutGuardsDrain(const TimeoutGuardsDrain &) = delete;
    auto operator=(const TimeoutGuardsDrain &) -> TimeoutGuardsDrain & = delete;
    TimeoutGuardsDrain(TimeoutGuardsDrain &&) = delete;
    auto operator=(TimeoutGuardsDrain &&) -> TimeoutGuardsDrain & = delete;
    // 守卫析构只做一件事：把残留看守全部推到期，触发的是本用例自己注册的续体（断言计数，不抛）。
    // sweep 内残余的抛出面只有向量分配一类 bad_alloc，测试期不做二次报告。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~TimeoutGuardsDrain() {
        aurora::detail::sweep_due_timeouts(std::chrono::steady_clock::now() + std::chrono::hours{1});
    }
};

}  // namespace

AURORA_TEST_CASE(async_delivers_value_to_then) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    std::promise<Result<int>> box;
    auto task = async([]() -> int { return 40 + 2; });
    task.then([&box](const Result<int> &r) -> void { box.set_value(r); });

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> r = fut.get();
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 42);
}

AURORA_TEST_CASE(async_accepts_result_returning_fn) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    // fn 返回 Result<T>：成功值与错误均原样透传（任务值类型萃取为 T，而非 Result<T>）。
    std::promise<Result<int>> ok_box;
    std::promise<Result<int>> err_box;
    auto ok_task = async([]() -> Result<int> { return Result<int>{7}; });
    auto err_task = async(
        []() -> Result<int> { return Result<int>{make_error(ErrorCode::GeneralInvalidArgument, std::string{"bad"})}; });
    ok_task.then([&ok_box](const Result<int> &r) -> void { ok_box.set_value(r); });
    err_task.then([&err_box](const Result<int> &r) -> void { err_box.set_value(r); });

    auto ok_fut = ok_box.get_future();
    AURORA_TEST_REQUIRE_EQ(ok_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> ok = ok_fut.get();
    AURORA_TEST_CHECK_TRUE(ok.ok());
    AURORA_TEST_CHECK_EQ(ok.value(), 7);

    auto err_fut = err_box.get_future();
    AURORA_TEST_REQUIRE_EQ(err_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> bad = err_fut.get();
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK(bad.error().code_enum == ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_STREQ(bad.error().message, "bad");
}

AURORA_TEST_CASE(async_captures_fn_exception_as_error) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    // fn 抛异常：invoke_safe 捕获并转为 runtime-async-exception 错误，不逃出 worker 线程。
    std::promise<Error> box;
    auto task = async([]() -> int { throw std::runtime_error{"boom"}; });
    task.then([&box](const Result<int> &r) -> void {
        if (!r.ok()) {
            box.set_value(r.error());
        }
    });

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Error e = fut.get();
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::RuntimeAsyncException);
    AURORA_TEST_CHECK_STREQ(e.code, "runtime-async-exception");
    AURORA_TEST_CHECK_THAT(e.message, m::has_substr("async task threw"));
    AURORA_TEST_CHECK_THAT(e.message, m::has_substr("boom"));
}

AURORA_TEST_CASE(async_cancel_drops_result_and_silences_callback) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    std::promise<void> entered;
    std::promise<void> release;
    std::promise<void> finished;
    auto entered_fut = entered.get_future();
    auto finished_fut = finished.get_future();

    auto task = async([&]() -> int {
        entered.set_value();
        release.get_future().wait();
        finished.set_value();
        return 7;
    });
    AURORA_TEST_REQUIRE_EQ(entered_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);

    AURORA_TEST_CHECK_FALSE(task.is_cancelled());
    task.cancel();
    AURORA_TEST_CHECK_TRUE(task.is_cancelled());

    // 取消后注册回调：补投条件含「未取消」，回调永不触发。
    std::atomic<bool> called{false};
    task.then([&called](const Result<int> &) -> void { called.store(true, std::memory_order_release); });

    release.set_value();
    AURORA_TEST_REQUIRE_EQ(finished_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_FALSE(called.load(std::memory_order_acquire));
}

AURORA_TEST_CASE(async_with_timeout_delivers_timeout_error) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    std::promise<void> fn_done;
    auto fn_done_fut = fn_done.get_future();

    auto task = async([&]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        fn_done.set_value();
        return 1;
    });
    task.with_timeout(std::chrono::milliseconds{100});

    std::promise<Error> err_box;
    task.then([&err_box](const Result<int> &r) -> void {
        if (!r.ok()) {
            err_box.set_value(r.error());
        }
    });

    auto err_fut = err_box.get_future();
    AURORA_TEST_REQUIRE_EQ(err_fut.wait_for(std::chrono::seconds{3}), std::future_status::ready);
    const Error e = err_fut.get();
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::RuntimeAsyncTimeout);
    AURORA_TEST_CHECK_STREQ(e.code, "async-timeout");
    AURORA_TEST_CHECK_THAT(e.message, m::has_substr("timed out"));

    // fn 仍会执行完毕（超时无法中断），等它收尾避免 worker 跨用例残留。
    AURORA_TEST_REQUIRE_EQ(fn_done_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
}

AURORA_TEST_CASE(async_completed_before_timeout_delivers_value) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_fut = entered.get_future();

    auto task = async([&]() -> int {
        entered.set_value();
        release.get_future().wait();
        return 1;
    });
    AURORA_TEST_REQUIRE_EQ(entered_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);

    // 超时先于完成注册：完成投递获胜，超时看守醒来后应空转。
    task.with_timeout(std::chrono::milliseconds{100});

    std::promise<Result<int>> box;
    task.then([&box](const Result<int> &r) -> void { box.set_value(r); });
    release.set_value();

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{3}), std::future_status::ready);
    const Result<int> r = fut.get();
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 1);

    // 有界等待超时看守醒来并空转（100ms），避免其跨用例残留。
    std::this_thread::sleep_for(std::chrono::milliseconds{130});
}

AURORA_TEST_CASE(timeout_guard_sweep_fires_only_due_entries) {
    // 登记式看守（deferred 路径的机制层）与线程无关，故在任意构建都要真跑。
    TimeoutGuardsDrain drain;
    auto due_flag = std::make_shared<std::atomic<bool>>(false);
    auto far_flag = std::make_shared<std::atomic<bool>>(false);
    const auto now = std::chrono::steady_clock::now();
    detail::register_timeout_guard(now - std::chrono::milliseconds{1},
                                   [due_flag]() -> void { due_flag->store(true, std::memory_order_release); });
    detail::register_timeout_guard(now + std::chrono::seconds{600},
                                   [far_flag]() -> void { far_flag->store(true, std::memory_order_release); });

    AURORA_TEST_CHECK_EQ(detail::sweep_due_timeouts(now), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(due_flag->load(std::memory_order_acquire));
    AURORA_TEST_CHECK_FALSE(far_flag->load(std::memory_order_acquire));
    // 已触发项出表，未触发项留表 ⇒ 唤醒决策只剩远处那一项。
    AURORA_TEST_CHECK_EQ(detail::sweep_due_timeouts(now), std::size_t{0});
    const double nearest = detail::next_timeout_deadline_ms(now);
    AURORA_TEST_CHECK_GT(nearest, 500000.0);
    AURORA_TEST_CHECK_LT(nearest, 601000.0);
}

AURORA_TEST_CASE(timeout_guard_deadline_reports_none_and_clamps_past) {
    TimeoutGuardsDrain drain;
    const auto now = std::chrono::steady_clock::now();
    AURORA_TEST_CHECK_EQ(detail::next_timeout_deadline_ms(now), -1.0);  // 空表 = 无看守
    auto noop = []() -> void {};
    detail::register_timeout_guard(now + std::chrono::milliseconds{500}, noop);
    detail::register_timeout_guard(now + std::chrono::milliseconds{50}, noop);
    detail::register_timeout_guard(now - std::chrono::milliseconds{500}, noop);  // 已过期
    AURORA_TEST_CHECK_EQ(detail::next_timeout_deadline_ms(now), 0.0);  // 钳 0：催本帧立即扫描
    detail::register_timeout_guard(now + std::chrono::milliseconds{200}, noop);
    AURORA_TEST_CHECK_EQ(detail::sweep_due_timeouts(now), std::size_t{1});  // 只摘已过期的那条
    const double nearest = detail::next_timeout_deadline_ms(now);
    AURORA_TEST_CHECK_GE(nearest, 40.0);  // 最小堆语义取最早：50ms 那条仍在表
    AURORA_TEST_CHECK_LE(nearest, 50.0);
}

AURORA_TEST_CASE(timeout_guard_expire_is_noop_once_delivered) {
    // 看守与任务结果的竞争由 delivered 单点裁决（worker 路径与登记路径共用 expire）。
    auto state = std::make_shared<detail::AsyncState<int>>();
    state->result = Result<int>{5};
    state->delivered = true;
    std::atomic<bool> called{false};
    state->on_done = [&called](const Result<int> &) -> void { called.store(true, std::memory_order_release); };
    detail::expire_timeout(state);
    AURORA_TEST_CHECK_FALSE(called.load(std::memory_order_acquire));
    AURORA_TEST_CHECK_TRUE(state->result->ok());  // 已投递的胜利结果不被超时改写
}

AURORA_TEST_CASE(then_replays_result_delivered_before_callback) {
    // `then()` 的补投分支：结果在 `on_done` 仍为空时就已 delivered，之后注册的回调必须拿到
    // 那次结果，否则永久悬空。用「超时先于 then 到期」造出该状态（正是它的现实来源之一），
    // 不依赖线程池时序，故无 pthreads 构建同样真跑。取 `Result<void>` 实例化：存储层的
    // `async_remove` 走的就是这一形态。补投经全局队列排队，由用例线程排空后执行。
    DutyOnly duty;
    AURORA_TEST_REQUIRE_TRUE(duty.duty.has_value());
    auto state = std::make_shared<detail::AsyncState<int>>();
    state->result = Result<int>{5};
    state->delivered = true;
    std::atomic<bool> ran{false};
    int got = 0;
    Task<int> task{state};
    task.then([&](const Result<int> &r) -> void {
        // 用例线程排空补投闭包时执行：断言落在值班（用例）线程。
        AURORA_TEST_CHECK_TRUE(r.ok());
        AURORA_TEST_CHECK_EQ(r.value(), 5);
        got = r.value();
        ran.store(true, std::memory_order_release);
    });
    AURORA_TEST_REQUIRE(duty.pump_until([&ran]() -> bool { return ran.load(std::memory_order_acquire); }));
    AURORA_TEST_CHECK_EQ(got, 5);  // 补投恰一次
}

AURORA_TEST_CASE(deferred_with_timeout_fires_at_frame_tail_sweep) {
#if AURORA_CAP_THREADS == 0
    // 无 pthreads 构建（浏览器 / 裸 Node）：任务只入队，看守也不能是睡在任务里的线程，
    // 超时只能由帧尾扫描（`Application::step_frame` 步骤 7）触发——本例直接驱动同一原语，
    // 不起真实 Application（Node 无 rAF，帧循环走回退分支，与本判据无关）。
    // 空唤醒租约令看守到点的回调滞留全局队列，由用例线程排空——与帧头排水等价。
    DutyOnly duty;
    std::atomic<int> calls{0};
    std::atomic<bool> timed_out{false};
    auto task = async([]() -> int { return 1; });  // 入队，无人跑
    task.with_timeout(std::chrono::milliseconds{5});
    task.then([&](const Result<int> &r) -> void {
        calls.fetch_add(1, std::memory_order_release);
        timed_out.store(!r.ok() && r.error().code_enum == ErrorCode::RuntimeAsyncTimeout, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});  // 越过期限
    AURORA_TEST_REQUIRE_EQ(detail::sweep_due_timeouts(std::chrono::steady_clock::now()), std::size_t{1});
    AURORA_TEST_REQUIRE(duty.pump_until([&calls]() -> bool { return calls.load(std::memory_order_acquire) >= 1; }));
    AURORA_TEST_CHECK_EQ(calls.load(std::memory_order_acquire), 1);
    AURORA_TEST_CHECK_TRUE(timed_out.load(std::memory_order_acquire));

    // 事后泵到真任务：结果照样算出，但 delivered 已置 ⇒ 不二次回调（超时改道后闭嘴）。
    AURORA_TEST_CHECK_EQ(ThreadPool::default_pool().pump(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(duty.pump_until([]() -> bool { return MainQueue::pending_count() == 0; }), true);
    AURORA_TEST_CHECK_EQ(calls.load(std::memory_order_acquire), 1);
#else
    AURORA_TEST_SKIP(
        "the deferred timeout path only holds in builds without pthreads "
        "(the native watchdog is a worker task; see async_with_timeout_delivers_timeout_error)");
#endif
}

AURORA_TEST_CASE(then_delivery_routes_through_main_queue) {
    AURORA_TEST_REQUIRE_THREADS();
    // 空唤醒租约令回调滞留全局队列：不排空就不执行，由用例线程统一捞起——
    // 验证投递走进程级唯一队列、在值班线程执行且恰一次。
    DutyOnly duty;
    AURORA_TEST_REQUIRE_TRUE(duty.duty.has_value());

    std::promise<void> entered;
    std::promise<void> release;
    std::promise<void> finished;
    auto entered_fut = entered.get_future();
    auto finished_fut = finished.get_future();

    auto task = async([&]() -> int {
        entered.set_value();
        release.get_future().wait();
        finished.set_value();
        return 9;
    });
    AURORA_TEST_REQUIRE_EQ(entered_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);

    std::atomic<bool> called{false};
    int got = -1;
    task.then([&](const Result<int> &r) -> void {
        // 回调由用例线程排水执行——断言落在值班（用例）线程，正是「主线程执行」的形态。
        called.store(true, std::memory_order_release);
        got = r.ok() ? r.value() : -1;
    });

    release.set_value();
    AURORA_TEST_REQUIRE_EQ(finished_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);

    // worker 完成 fn 之后才把投递排进队列：轮询等待（有界）。
    AURORA_TEST_REQUIRE(wait_until([]() -> bool { return MainQueue::pending_count() > 0; }));

    // 用例线程排空 → 回调在值班线程执行。
    for (auto &fn : MainQueue::take_batch()) {
        if (fn) {
            fn();
        }
    }
    AURORA_TEST_CHECK_TRUE(called.load(std::memory_order_acquire));
    AURORA_TEST_CHECK_EQ(got, 9);
    AURORA_TEST_CHECK_EQ(MainQueue::pending_count(), std::size_t{0});  // delivered 去重：无二次投递
}

AURORA_TEST_CASE(cancel_observer_fires_once_on_the_first_cancel) {
    auto state = std::make_shared<detail::AsyncState<int>>();
    Task<int> task{state};
    std::atomic<int> hits{0};
    task.on_cancel([&hits]() -> void { hits.fetch_add(1, std::memory_order_relaxed); });

    AURORA_TEST_CHECK_EQ(hits.load(), 0);  // 登记本身不算一次触发
    task.cancel();
    AURORA_TEST_CHECK_TRUE(task.is_cancelled());
    AURORA_TEST_CHECK_EQ(hits.load(), 1);
    task.cancel();  // 幂等：第二次既不重新置位，也不再触发观察者
    AURORA_TEST_CHECK_EQ(hits.load(), 1);
}

AURORA_TEST_CASE(cancel_observer_registered_after_cancel_fires_immediately) {
    auto state = std::make_shared<detail::AsyncState<int>>();
    Task<int> task{state};
    task.cancel();
    std::atomic<int> hits{0};
    task.on_cancel([&hits]() -> void { hits.fetch_add(1, std::memory_order_relaxed); });
    // 竞态纪律：取消先于登记也要有观察者收到这一次事实，不留「登记即失效」的窗口。
    AURORA_TEST_CHECK_EQ(hits.load(), 1);
    task.on_cancel({});  // 已取消后撤销登记不该补触发，也不该改变既成事实
    AURORA_TEST_CHECK_EQ(hits.load(), 1);
}

AURORA_TEST_CASE(cancel_observer_runs_outside_the_state_lock) {
    auto state = std::make_shared<detail::AsyncState<int>>();
    Task<int> task{state};
    std::atomic<bool> lock_free{false};
    task.on_cancel([&state, &lock_free]() -> void {
        // try_lock 拿得到即证明钩子不在持锁区间里跑（拿到立刻放手）；钩子要能回头读这枚状态，
        // 否则「在钩子里投递取消事实」这类实现一落码就自死锁。
        if (state->mutex.try_lock()) {
            state->mutex.unlock();
            lock_free.store(true, std::memory_order_release);
        }
    });
    task.cancel();
    AURORA_TEST_CHECK_TRUE(lock_free.load(std::memory_order_acquire));
}

AURORA_TEST_CASE(cancel_observer_keeps_only_the_last_registration) {
    auto state = std::make_shared<detail::AsyncState<int>>();
    Task<int> task{state};
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    task.on_cancel([&first]() -> void { first.fetch_add(1, std::memory_order_relaxed); });
    task.on_cancel([&second]() -> void { second.fetch_add(1, std::memory_order_relaxed); });
    task.cancel();
    AURORA_TEST_CHECK_EQ(first.load(), 0);  // 只保留最后登记的一枚
    AURORA_TEST_CHECK_EQ(second.load(), 1);

    // 传空即撤销登记：此后的取消不再触发任何人。
    auto fresh = std::make_shared<detail::AsyncState<int>>();
    Task<int> cleared{fresh};
    std::atomic<int> hits{0};
    cleared.on_cancel([&hits]() -> void { hits.fetch_add(1, std::memory_order_relaxed); });
    cleared.on_cancel({});
    cleared.cancel();
    AURORA_TEST_CHECK_EQ(hits.load(), 0);
}

AURORA_TEST_CASE(cancel_observer_is_silent_on_completion_and_timeout) {
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    // 正常落定与超时改道都不是取消：观察者只在 `cancel()` 那一刻说话。
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_fut = entered.get_future();
    std::atomic<int> hits{0};

    auto task = async([&entered, &release]() -> int {
        entered.set_value();
        release.get_future().wait();
        return 3;
    });
    task.on_cancel([&hits]() -> void { hits.fetch_add(1, std::memory_order_relaxed); });
    AURORA_TEST_REQUIRE_EQ(entered_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    task.with_timeout(std::chrono::milliseconds{1});  // 到点改道超时错误，不是取消
    std::atomic<bool> delivered{false};
    task.then([&delivered](const Result<int> &) -> void { delivered.store(true, std::memory_order_release); });
    release.set_value();
    AURORA_TEST_REQUIRE(wait_until([&delivered]() -> bool { return delivered.load(std::memory_order_acquire); }));
    AURORA_TEST_CHECK_EQ(hits.load(), 0);
    AURORA_TEST_CHECK_FALSE(task.is_cancelled());
}

AURORA_TEST_CASE(with_timeout_delivers_error_after_interruptible_wait_change) {
    // 回归用例：非 deferred 下 with_timeout 改走 interruptible_wait_for，
    // 超时仍正确改道为 async-timeout 错误（语义不变，仅等待可被进程退出中断）。
    AURORA_TEST_REQUIRE_THREADS();
    DutyPump duty;  // 发起需值班在岗；回投经后台排水落定，用例只等 future/原子量
    auto task = async([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        return 1;
    });
    task.with_timeout(std::chrono::milliseconds{100});
    std::promise<Error> err_box;
    task.then([&err_box](const Result<int> &r) -> void {
        if (!r.ok()) {
            err_box.set_value(r.error());
        }
    });
    auto err_fut = err_box.get_future();
    AURORA_TEST_REQUIRE_EQ(err_fut.wait_for(std::chrono::seconds{3}), std::future_status::ready);
    AURORA_TEST_CHECK(err_fut.get().code_enum == ErrorCode::RuntimeAsyncTimeout);
}

AURORA_TEST_CASE(blocking_wait_returns_result_without_drain) {
    AURORA_TEST_REQUIRE_THREADS();
    // 打烊取消的是投递、不是结果：回调滞留队列（排水才执行），等待者不等回调、只等结果本身。
    DutyOnly duty;
    AURORA_TEST_REQUIRE_TRUE(duty.duty.has_value());
    std::atomic<bool> cb_ran{false};
    auto task = async([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return 33;
    });
    task.then([&cb_ran](const Result<int> &) -> void { cb_ran.store(true, std::memory_order_release); });

    const Result<int> r = task.blocking_wait();  // 不排空也拿到结果（代价：停摆到有值）
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 33);
    AURORA_TEST_CHECK_FALSE(cb_ran.load(std::memory_order_acquire));  // 回调是投递语义：没排水就不该跑
    duty.pump_until([&cb_ran]() -> bool { return cb_ran.load(std::memory_order_acquire); });
}

AURORA_TEST_CASE(blocking_wait_after_closure_still_yields_result) {
    AURORA_TEST_REQUIRE_THREADS();
    // 结果落地晚于打烊：回投被打烊取消（then 永不执行），但 worker 照常落结果——
    // 句柄持有者经 blocking_wait 取到本值。用 release→wait→re-lease 造出真实打烊窗。
    DutyOnly duty;
    auto &held = require_ref(duty.duty);
    const auto epoch = MainQueue::current_epoch();
    std::atomic<bool> cb_ran{false};
    auto task = async([&]() -> int {
        // 让 worker 慢到「用例已还租约、且第二次上岗完成」之后再落值，确保回投落在失配世代。
        while (MainQueue::current_epoch() == epoch) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return 77;
    });
    task.then([&cb_ran](const Result<int> &) -> void { cb_ran.store(true, std::memory_order_release); });

    held.release();
    auto next = MainQueue::on_duty([]() -> void {});  // 新世界：旧世回投必然世代失配
    auto &next_held = require_ref(next);

    const Result<int> r = task.blocking_wait();
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 77);
    AURORA_TEST_CHECK_FALSE(cb_ran.load(std::memory_order_acquire));  // 回调随打烊取消
    next_held.release();
}

AURORA_TEST_CASE(cancel_at_birth_task_short_circuits_every_observable) {
    // 出生即取消：打烊窗口内发起的 Task 不排工、即刻呈已取消——then 恒不执行、
    // on_cancel 登记即触发、blocking_wait 立即返回取消错误（绝不空等永不到来的结果）。
    auto task = [&]() -> Task<int> {
        const auto window = MainQueue::closing_scope();
        auto t = async([]() -> int { return 1; });
        AURORA_TEST_CHECK_TRUE(t.is_cancelled());
        std::atomic<bool> then_ran{false};
        t.then([&then_ran](const Result<int> &) -> void { then_ran.store(true, std::memory_order_release); });
        AURORA_TEST_CHECK_FALSE(then_ran.load(std::memory_order_acquire));  // 补投条件含「未取消」
        std::atomic<int> cancel_hits{0};
        t.on_cancel([&cancel_hits]() -> void { cancel_hits.fetch_add(1, std::memory_order_relaxed); });
        AURORA_TEST_CHECK_EQ(cancel_hits.load(), 1);  // 登记时已取消即当场触发（既有契约）
        const Result<int> r = t.blocking_wait();  // 已落取消终值：立即返回，不挂死
        AURORA_TEST_CHECK_FALSE(r.ok());
        AURORA_TEST_CHECK(r.error().code_enum == ErrorCode::RuntimeCancelled);
        static_cast<void>(window);
        return t;
    }();
    AURORA_TEST_CHECK_TRUE(task.is_cancelled());
}

AURORA_TEST_CASE(initiation_without_a_duty_loop_hard_fails_through_the_handler) {
    // 非打烊、无值班发起是纯编程错误：走 `on_contract_violation`（FATAL 日志 + 注入的处理器，
    // 缺省 terminate）。处理器抛出哨兵异常，「命中且不 terminate」才成为可断言的判据。
    struct ContractViolation {};
    auto hits = std::make_shared<std::atomic<int>>(0);
    aurora::set_strict_failure_handler([hits](std::string_view) -> void {
        hits->fetch_add(1, std::memory_order_relaxed);
        throw ContractViolation{};
    });
    AURORA_TEST_CHECK_ANY_THROW(static_cast<void>(async([]() -> int { return 1; })));
    aurora::set_strict_failure_handler(nullptr);  // 判定先行复位：进程级处理器勿留给后续用例
    AURORA_TEST_CHECK_EQ(hits->load(std::memory_order_relaxed), 1);
}

}  // namespace aurora::test_cases::utest_async
