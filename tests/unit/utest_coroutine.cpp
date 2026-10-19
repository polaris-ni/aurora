/// 测试类型: unit
/// 目标单元: include/aurora/state/coroutine.h
/// 测试说明: CoroTask<T>/CoroTask<void> 的完成与结果语义、co_await co_async 的值/异常/Result
/// 错误透传、同步协程立即完成，以及续体经主线程投递器恢复（全部经 promise/future 有界等待，断言只在用例线程执行）；
/// 另有 `on_done()` 完成观察者的四条契约——收尾恰触发一次、已收尾时当场触发、只留最后登记的一枚
/// （传空即撤销）、触发后不留下持有（观察者摘出即清空，会话与共享状态之间不成自持环）

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

#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_coroutine {

namespace m = aurora::testing::matchers;

namespace {

// @brief 有界轮询：每 1ms 轮询一次 pred，超时返回最后一次判定（禁止无界阻塞）。
template <typename Pred>
// pred 在轮询循环中被多次调用，转发（std::move/forward）会导致后续迭代使用已移动对象，故有意不转发。
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

/// @brief 排空主线程投递队列直到 `done()` 成立（每轮先换出批次再在用例线程执行，锁外调用）。
///
/// 队列就是「帧还没收尾」的可控现场：续体排在队列里，不排空就不会收尾，于是登记观察者的先后次序
/// 能用单线程时序钉死。空队列时睡 1 ms 再看（禁止忙等），预算用完仍不成立就把失败交回调用方的 REQUIRE。
/// @param queue_mutex 队列锁。
/// @param queued 投递队列（与安装的投递器同一份存储）。
/// @param done 收尾判据。
/// @param budget 等待预算。
/// @return 判据成立为真。
[[nodiscard]] auto drain_until_done(std::mutex &queue_mutex, std::vector<std::function<void()>> &queued,
                                    const std::function<bool()> &done,
                                    std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    bool satisfied = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<std::function<void()>> batch;
        {
            std::scoped_lock lock(queue_mutex);
            batch.swap(queued);
        }
        for (std::function<void()> &fn : batch) {
            fn();
        }
        satisfied = done();
        if (satisfied) {
            break;
        }
        if (batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});  // 续体还没排进队列
        }
    }
    return satisfied;
}

}  // namespace

AURORA_TEST_CASE(co_async_delivers_value_to_await) {
    AURORA_TEST_REQUIRE_THREADS();
    std::promise<Result<int>> box;
    // 闭包为用例局部变量，协程在返回帧销毁前经 wait_until 确认完成，闭包存活期覆盖协程生命周期。
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&box]() -> CoroTask<int> {
        Result<int> r = co_await co_async([]() -> int { return 40 + 2; });
        box.set_value(r);
        co_return r.value();
    };
    const auto task = launch(coro());

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> r = fut.get();
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 42);

    // co_await 之后协程 co_return 收尾：轮询等待帧销毁置位 done。
    AURORA_TEST_CHECK(wait_until([&task]() -> bool { return task.is_done(); }));
    AURORA_TEST_CHECK_EQ(task.result().value(), 42);
}

AURORA_TEST_CASE(co_async_captures_fn_exception_as_error) {
    AURORA_TEST_REQUIRE_THREADS();
    // fn 抛异常：invoke_safe 转为 runtime-async-exception 错误，co_await 表达式不抛。
    std::promise<Error> box;
    // 闭包为用例局部变量，协程在返回帧销毁前经 wait_until 确认完成，闭包存活期覆盖协程生命周期。
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&box]() -> CoroTask<int> {
        Result<int> r = co_await co_async([]() -> int { throw std::runtime_error{"inner boom"}; });
        if (!r.ok()) {
            box.set_value(r.error());
        }
        co_return 0;
    };
    const auto task = launch(coro());

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Error e = fut.get();
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::RuntimeAsyncException);
    AURORA_TEST_CHECK_STREQ(e.code, "runtime-async-exception");
    AURORA_TEST_CHECK_THAT(e.message, m::has_substr("async task threw"));
    AURORA_TEST_CHECK_THAT(e.message, m::has_substr("inner boom"));
    AURORA_TEST_CHECK(wait_until([&task]() -> bool { return task.is_done(); }));
}

AURORA_TEST_CASE(co_async_preserves_result_error_from_fn) {
    AURORA_TEST_REQUIRE_THREADS();
    // fn 返回错误 Result：原样透传，不经异常包装。
    std::promise<Error> box;
    // 闭包为用例局部变量，协程在返回帧销毁前经 wait_until 确认完成，闭包存活期覆盖协程生命周期。
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&box]() -> CoroTask<int> {
        Result<int> r = co_await co_async([]() -> Result<int> {
            return Result<int>{make_error(ErrorCode::GeneralInvalidArgument, std::string{"passthrough boom"})};
        });
        if (!r.ok()) {
            box.set_value(r.error());
        }
        co_return 0;
    };
    const auto task = launch(coro());

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Error e = fut.get();
    AURORA_TEST_CHECK(e.code_enum == ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_STREQ(e.message, "passthrough boom");
    AURORA_TEST_CHECK(wait_until([&task]() -> bool { return task.is_done(); }));
}

AURORA_TEST_CASE(coroutine_void_success_and_exception_paths) {
    // 成功：co_return 无值，error() 为空。
    const auto ok_task = launch([]() -> CoroTask<void> { co_return; }());
    AURORA_TEST_CHECK_TRUE(ok_task.is_done());
    AURORA_TEST_CHECK_FALSE(ok_task.error().has_value());

    // 异常：unhandled_exception 捕获并转为 runtime-coroutine-exception 错误。
    // （经参数把 throw 变为条件路径，确保 lambda 含 co_return 而成为真正的协程。）
    const auto throw_fn = [](bool boom) -> CoroTask<void> {
        if (boom) {
            throw std::runtime_error{"void boom"};
        }
        co_return;
    };
    const auto bad_task = launch(throw_fn(true));
    AURORA_TEST_CHECK_TRUE(bad_task.is_done());
    const std::optional<Error> err = bad_task.error();
    AURORA_TEST_REQUIRE_TRUE(err.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(err.value().code_enum == ErrorCode::RuntimeCoroutineException);
    AURORA_TEST_CHECK_STREQ(err.value().code, "runtime-coroutine-exception");
    AURORA_TEST_CHECK_THAT(err.value().message, m::has_substr("coroutine threw"));
    AURORA_TEST_CHECK_THAT(err.value().message, m::has_substr("void boom"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(value_coroutine_exception_folds_into_error_result) {
    // T 版 promise 的 unhandled_exception：std::exception 与非 std::exception 两分支。
    const auto boom_fn = [](int mode) -> CoroTask<int> {
        if (mode == 1) {
            throw 42;  // 非 std::exception：走 catch (...) 分支
        }
        if (mode == 2) {
            throw std::runtime_error{"int boom"};
        }
        co_return 0;
    };
    const auto std_task = launch(boom_fn(2));
    AURORA_TEST_CHECK_TRUE(std_task.is_done());
    const Result<int> std_res = std_task.result();
    AURORA_TEST_CHECK_FALSE(std_res.ok());
    AURORA_TEST_CHECK(std_res.error().code_enum == ErrorCode::RuntimeCoroutineException);
    AURORA_TEST_CHECK_THAT(std_res.error().message, m::has_substr("int boom"));

    const auto int_task = launch(boom_fn(1));
    AURORA_TEST_CHECK_TRUE(int_task.is_done());
    const Result<int> int_res = int_task.result();
    AURORA_TEST_CHECK_FALSE(int_res.ok());
    AURORA_TEST_CHECK_THAT(int_res.error().message, m::has_substr("unknown exception"));
}

AURORA_TEST_CASE(void_coroutine_non_std_exception_folds_into_unknown_error) {
    // void 版 promise 的 catch (...) 分支：抛非 std::exception 对象。
    const auto throw_fn = [](bool boom) -> CoroTask<void> {
        if (boom) {
            throw 7;  // NOLINT(*-throw-missing-move) 非 std::exception 触发未知异常分支
        }
        co_return;
    };
    const auto bad_task = launch(throw_fn(true));
    AURORA_TEST_CHECK_TRUE(bad_task.is_done());
    const std::optional<Error> err = bad_task.error();
    AURORA_TEST_REQUIRE_TRUE(err.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(err.value().code_enum == ErrorCode::RuntimeCoroutineException);
    AURORA_TEST_CHECK_THAT(err.value().message, m::has_substr("unknown exception"));
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(synchronous_coroutine_completes_with_result) {
    // 无挂起点：协程在调用表达式内同步跑完（final_suspend 即毁帧并置位 done）。
    const auto task = launch([]() -> CoroTask<int> { co_return 42; }());
    AURORA_TEST_CHECK_TRUE(task.is_done());
    const Result<int> r = task.result();
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 42);
}

AURORA_TEST_CASE(continuation_resumes_through_main_poster) {
    AURORA_TEST_REQUIRE_THREADS();
    // 投递器把 resume 排队，由用例线程排空：验证续体经主线程投递器恢复。
    std::mutex queue_mutex;
    std::vector<std::function<void()>> queued;
    Task<int>::set_main_poster([&](std::function<void()> fn) -> void {
        std::scoped_lock lock(queue_mutex);
        queued.push_back(std::move(fn));
    });
    MainPosterGuard guard;

    std::promise<int> box;
    // 闭包为用例局部变量，续体仅由本用例线程排空队列后恢复，且协程完成在用例退出前有轮询确认，
    // 闭包存活期覆盖协程整个生命周期。
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    auto coro = [&box]() -> CoroTask<int> {
        Result<int> r = co_await co_async([]() -> int { return 7; });
        box.set_value(r.ok() ? r.value() : -1);
        co_return 0;
    };
    const auto task = launch(coro());

    // worker 完成 fn 后把 resume 排进队列：轮询等待（有界）。
    AURORA_TEST_REQUIRE(wait_until([&]() -> bool {
        std::scoped_lock lock(queue_mutex);
        return !queued.empty();
    }));

    // 用例线程排空 → 续体在「主线程」恢复。
    std::vector<std::function<void()>> batch;
    {
        std::scoped_lock lock(queue_mutex);
        batch.swap(queued);
    }
    for (std::function<void()> &fn : batch) {
        fn();
    }

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 7);
    AURORA_TEST_CHECK(wait_until([&task]() -> bool { return task.is_done(); }));
}

AURORA_TEST_CASE(on_done_reports_the_value_the_frame_ends_with) {
    AURORA_TEST_REQUIRE_THREADS();
    std::promise<Result<int>> box;
    auto task = []() -> CoroTask<int> {
        Result<int> r = co_await co_async([]() -> int { return 11; });
        co_return r.ok() ? r.value() : -1;
    }();
    task.on_done([&box](const Result<int> &r) -> void { box.set_value(r); });

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> r = fut.get();
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value(), 11);
}

AURORA_TEST_CASE(on_done_fires_at_registration_when_the_frame_already_ended) {
    // 无挂起点的协程在创建表达式内就收尾（final_suspend 即毁帧并置位 done）：此后登记观察者
    // 必须当场在登记线程触发一次，不留「登记即失效」的窗口。
    auto task = []() -> CoroTask<int> { co_return 42; }();
    AURORA_TEST_CHECK_TRUE(task.is_done());

    bool fired = false;
    int got = 0;
    task.on_done([&fired, &got](const Result<int> &r) -> void {
        fired = true;
        got = r.value();
    });
    AURORA_TEST_CHECK_TRUE(fired);
    AURORA_TEST_CHECK_EQ(got, 42);
}

AURORA_TEST_CASE(on_done_carries_the_error_of_a_frame_ended_by_exception) {
    AURORA_TEST_REQUIRE_THREADS();
    std::promise<Result<int>> box;
    auto task = []() -> CoroTask<int> {
        (void)co_await co_async([]() -> int { return 1; });
        throw std::runtime_error{"outer boom"};
    }();
    task.on_done([&box](const Result<int> &r) -> void { box.set_value(r); });

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    const Result<int> r = fut.get();
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK(r.error().code_enum == ErrorCode::RuntimeCoroutineException);
    AURORA_TEST_CHECK_STREQ(r.error().code, "runtime-coroutine-exception");
    AURORA_TEST_CHECK_THAT(r.error().message, m::has_substr("outer boom"));
}

AURORA_TEST_CASE(on_done_keeps_only_the_last_registration) {
    AURORA_TEST_REQUIRE_THREADS();
    // 投递器把续体排进队列：不排空就不会收尾，两次登记的先后次序因此是单线程可控的。
    std::mutex queue_mutex;
    std::vector<std::function<void()>> queued;
    Task<int>::set_main_poster([&](std::function<void()> fn) -> void {
        std::scoped_lock lock(queue_mutex);
        queued.push_back(std::move(fn));
    });
    MainPosterGuard guard;

    std::atomic<int> first{0};
    std::atomic<int> second{0};
    auto task = []() -> CoroTask<int> {
        (void)co_await co_async([]() -> int { return 1; });
        co_return 1;
    }();
    task.on_done([&first](const Result<int> &) -> void { first.fetch_add(1, std::memory_order_relaxed); });
    task.on_done([&second](const Result<int> &) -> void { second.fetch_add(1, std::memory_order_relaxed); });
    AURORA_TEST_REQUIRE(drain_until_done(queue_mutex, queued, [&task]() -> bool { return task.is_done(); }));
    AURORA_TEST_CHECK_EQ(first.load(), 0);  // 只保留最后登记的一枚
    AURORA_TEST_CHECK_EQ(second.load(), 1);

    // 传空即撤销登记：收尾时没有任何观察者会被触发。
    std::atomic<int> cleared{0};
    auto another = []() -> CoroTask<int> {
        (void)co_await co_async([]() -> int { return 2; });
        co_return 2;
    }();
    another.on_done([&cleared](const Result<int> &) -> void { cleared.fetch_add(1, std::memory_order_relaxed); });
    another.on_done({});
    AURORA_TEST_REQUIRE(drain_until_done(queue_mutex, queued, [&another]() -> bool { return another.is_done(); }));
    AURORA_TEST_CHECK_EQ(cleared.load(), 0);
}

AURORA_TEST_CASE(on_done_releases_its_own_capture_once_it_has_fired) {
    AURORA_TEST_REQUIRE_THREADS();
    // 观察者是「摘出即清空」而不是留在共享状态里：留着它，会话持有共享状态、共享状态又持有会话
    // 就成了自持环——适配器再也放不掉那条交换。sentinel 的引用计数就是这一条的可读判据。
    auto sentinel = std::make_shared<std::atomic<int>>(0);
    std::atomic<bool> fired{false};
    {
        auto task = []() -> CoroTask<int> {
            (void)co_await co_async([]() -> int { return 5; });
            co_return 5;
        }();
        task.on_done([sentinel, &fired](const Result<int> &) -> void {
            (void)sentinel;  // 观察者自持一份：它被留着就说明环没断
            fired.store(true, std::memory_order_release);
        });
        AURORA_TEST_REQUIRE(wait_until([&fired]() -> bool { return fired.load(std::memory_order_acquire); }));
        // 判据按引用取 sentinel：按值捕获会给轮询者自己加一份持有，那就永远比不到「只剩用例这一份」。
        AURORA_TEST_CHECK(wait_until([&sentinel]() -> bool { return sentinel.use_count() == 1; }));
    }
    AURORA_TEST_CHECK_EQ(sentinel.use_count(), 1);  // 帧与句柄都已销毁：没有第二份持有者残留
}

}  // namespace aurora::test_cases::utest_coroutine
