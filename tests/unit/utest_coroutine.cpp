/// 测试类型: unit
/// 目标单元: include/aurora/state/coroutine.h
/// 测试说明: CoroTask<T>/CoroTask<void> 的完成与结果语义、co_await co_async 的值/异常/Result
/// 错误透传、同步协程立即完成，以及续体经主线程投递器恢复（全部经 promise/future 有界等待，断言只在用例线程执行）

#include <chrono>
#include <functional>
#include <future>
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

/// @brief 有界轮询：每 1ms 轮询一次 pred，超时返回最后一次判定（禁止无界阻塞）。
template <typename Pred>
// pred 在轮询循环中被多次调用，转发（std::move/forward）会导致后续迭代使用已移动对象，故有意不转发。
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
auto wait_until(Pred&& pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
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
    MainPosterGuard(const MainPosterGuard&) = delete;
    auto operator=(const MainPosterGuard&) -> MainPosterGuard& = delete;
    MainPosterGuard(MainPosterGuard&&) = delete;
    auto operator=(MainPosterGuard&&) -> MainPosterGuard& = delete;
    ~MainPosterGuard() { aurora::Task<int>::set_main_poster(nullptr); }
};

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
    for (std::function<void()>& fn : batch) {
        fn();
    }

    auto fut = box.get_future();
    AURORA_TEST_REQUIRE_EQ(fut.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 7);
    AURORA_TEST_CHECK(wait_until([&task]() -> bool { return task.is_done(); }));
}

}  // namespace aurora::test_cases::utest_coroutine
