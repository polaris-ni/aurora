/// 测试类型: unit
/// 目标单元: include/aurora/state/async.h
/// 测试说明: Task<T> 的 then 值投递、Result 透传与异常捕获、cancel 丢弃结果、with_timeout
/// 超时投递与提前完成不误报、结果就绪后注册回调的补投、主线程投递器路由，以及
/// deferred 构建的登记式超时看守（到期表语义：只触发过期项、唤醒期限取最早且过期钳 0、
/// delivered 后改道不二次回调）。全部经 promise/future 或原子量有界等待，断言只在用例线程执行

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"  // AURORA_CAP_THREADS（deferred 分支的编译期判据）
#include "aurora/state/async.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_async {

namespace m = aurora::testing::matchers;

namespace {

/// @brief 有界轮询：每 1ms 轮询一次 pred，超时返回最后一次判定（禁止无界阻塞）。
template <typename Pred>
// pred 在轮询循环内可能被多次调用，不能按「一次性转发」用 std::forward（对带状态可调用体
// 转成右值引用会误移动，破坏后续再次调用），刻意始终以左值形式反复调用，故抑制该告警。
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
auto wait_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

/// @brief 用例退出（含 REQUIRE 中止）时把主线程投递器恢复为默认直调，避免污染同进程后续用例。
struct MainPosterGuard {
    MainPosterGuard() = default;
    MainPosterGuard(const MainPosterGuard &) = delete;
    auto operator=(const MainPosterGuard &) -> MainPosterGuard & = delete;
    MainPosterGuard(MainPosterGuard &&) = delete;
    auto operator=(MainPosterGuard &&) -> MainPosterGuard & = delete;
    ~MainPosterGuard() { aurora::Task<int>::set_main_poster(nullptr); }
};

/// @brief 用例退出时清空全局看守表：条目只在到期时被摘除，未到期项若跨用例残留，
///        后续任何帧尾扫描都会打到本用例的栈上对象。以「远超期限的 now」扫一趟即全清。
struct TimeoutGuardsDrain {
    TimeoutGuardsDrain() = default;
    TimeoutGuardsDrain(const TimeoutGuardsDrain&) = delete;
    auto operator=(const TimeoutGuardsDrain&) -> TimeoutGuardsDrain& = delete;
    TimeoutGuardsDrain(TimeoutGuardsDrain&&) = delete;
    auto operator=(TimeoutGuardsDrain&&) -> TimeoutGuardsDrain& = delete;
    ~TimeoutGuardsDrain() {
        aurora::detail::sweep_due_timeouts(std::chrono::steady_clock::now() + std::chrono::hours{1});
    }
};

}  // namespace

AURORA_TEST_CASE(async_delivers_value_to_then) {
    AURORA_TEST_REQUIRE_THREADS();
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
    detail::register_timeout_guard(now - std::chrono::milliseconds{1}, [due_flag]() -> void {
        due_flag->store(true, std::memory_order_release);
    });
    detail::register_timeout_guard(now + std::chrono::seconds{600}, [far_flag]() -> void {
        far_flag->store(true, std::memory_order_release);
    });

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
    AURORA_TEST_CHECK_GE(nearest, 40.0);   // 最小堆语义取最早：50ms 那条仍在表
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

AURORA_TEST_CASE(deferred_with_timeout_fires_at_frame_tail_sweep) {
#if AURORA_CAP_THREADS == 0
    // 无 pthreads 构建（浏览器 / 裸 Node）：任务只入队，看守也不能是睡在任务里的线程，
    // 超时只能由帧尾扫描（`Application::step_frame` 步骤 7）触发——本例直接驱动同一原语，
    // 不起真实 Application（Node 无 rAF，帧循环走回退分支，与本判据无关）。
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
    AURORA_TEST_CHECK_EQ(calls.load(std::memory_order_acquire), 1);
    AURORA_TEST_CHECK_TRUE(timed_out.load(std::memory_order_acquire));

    // 事后泵到真任务：结果照样算出，但 delivered 已置 ⇒ 不二次回调（超时改道后闭嘴）。
    AURORA_TEST_CHECK_EQ(ThreadPool::default_pool().pump(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(calls.load(std::memory_order_acquire), 1);
#else
    AURORA_TEST_SKIP("deferred 超时路径仅在无 pthreads 构建成立（native 看守是 worker 任务，"
                     "见 async_with_timeout_delivers_timeout_error）");
#endif
}

AURORA_TEST_CASE(then_delivery_routes_through_main_poster) {
    AURORA_TEST_REQUIRE_THREADS();
    // 投递器把回调排队，由用例线程统一排空：验证投递走主线程投递器且恰一次。
    std::mutex queue_mutex;
    std::vector<std::function<void()>> queued;
    Task<int>::set_main_poster([&](std::function<void()> fn) -> void {
        std::scoped_lock lock(queue_mutex);
        queued.push_back(std::move(fn));
    });
    MainPosterGuard guard;

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
        called.store(true, std::memory_order_release);
        got = r.ok() ? r.value() : -1;
    });

    release.set_value();
    AURORA_TEST_REQUIRE_EQ(finished_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);

    // worker 完成 fn 之后才把投递排进队列：轮询等待（有界）。
    AURORA_TEST_REQUIRE(wait_until([&]() -> bool {
        std::scoped_lock lock(queue_mutex);
        return !queued.empty();
    }));

    // 用例线程排空 → 回调在「主线程」执行。
    std::vector<std::function<void()>> batch;
    {
        std::scoped_lock lock(queue_mutex);
        batch.swap(queued);
    }
    for (std::function<void()> &fn : batch) {
        fn();
    }
    AURORA_TEST_CHECK_TRUE(called.load(std::memory_order_acquire));
    AURORA_TEST_CHECK_EQ(got, 9);

    std::scoped_lock lock(queue_mutex);
    AURORA_TEST_CHECK_TRUE(queued.empty());  // delivered 去重：无二次投递
}

}  // namespace aurora::test_cases::utest_async
