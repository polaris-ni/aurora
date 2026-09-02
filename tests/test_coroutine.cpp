// test_coroutine.cpp — 覆盖 au::co_async / CoroTask / launch：取值、错误路径、超时（无 poster 直接 resume）。

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::co_async;
using aurora::CoroTask;
using aurora::ErrorCode;
using aurora::Result;

using std::chrono_literals::operator""ms;

namespace {
void wait_until(std::atomic<bool> const &flag, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!flag.load() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(5ms);
    }
}
} // namespace

// 协程：后台计算后把结果写入共享存储。
static auto coro_ok(std::shared_ptr<Result<int>> out, std::shared_ptr<std::atomic<bool>> done) -> CoroTask<void> {
    const Result<int> r = co_await co_async([]() -> int { return 21 * 2; });
    *out = r;
    done->store(true);
    co_return;
}

// 协程：fn 返回错误（或抛异常）→ await 求得错误 Result。
static auto coro_err(std::shared_ptr<Result<int>> out, std::shared_ptr<std::atomic<bool>> done) -> CoroTask<void> {
    const Result<int> r =
        co_await co_async([]() -> Result<int> { return make_error(ErrorCode::GeneralUnknown, "nope"); });
    *out = r;
    done->store(true);
    co_return;
}

// 协程：抛异常 fn → 捕获为 async-exception。
static auto coro_throw(std::shared_ptr<Result<int>> out, std::shared_ptr<std::atomic<bool>> done) -> CoroTask<void> {
    const Result<int> r = co_await co_async([]() -> int { throw std::runtime_error("x"); });
    *out = r;
    done->store(true);
    co_return;
}

AURORA_TEST() {
    // 1) 成功取值：co_await 求得后台计算结果。
    {
        const auto out = std::make_shared<Result<int>>(0);
        const auto done = std::make_shared<std::atomic<bool>>(false);
        launch(coro_ok(out, done));
        wait_until(*done, 1000ms);
        AURORA_TEST_CHECK(done->load());
        AURORA_TEST_CHECK(out->ok() && out->value() == 42);
    }

    // 2) 错误路径：co_await 求得错误 Result。
    {
        const auto out = std::make_shared<Result<int>>(0);
        const auto done = std::make_shared<std::atomic<bool>>(false);
        launch(coro_err(out, done));
        wait_until(*done, 1000ms);
        AURORA_TEST_CHECK(done->load());
        AURORA_TEST_CHECK(!out->ok());
        AURORA_TEST_CHECK(out->error().code == "general-unknown");
    }

    // 3) 异常路径：fn 抛异常 → async-exception。
    {
        const auto out = std::make_shared<Result<int>>(0);
        const auto done = std::make_shared<std::atomic<bool>>(false);
        launch(coro_throw(out, done));
        wait_until(*done, 1000ms);
        AURORA_TEST_CHECK(done->load());
        AURORA_TEST_CHECK(!out->ok());
        AURORA_TEST_CHECK(out->error().code == "runtime-async-exception");
    }

    // 4) 顺序执行：两个 co_await 先后完成。
    {
        auto acc = std::make_shared<std::atomic<int>>(0);
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto seq = [acc, done]() -> CoroTask<void> {
            Result<int> a = co_await co_async([]() -> int { return 10; });
            if (a) {
                acc->fetch_add(a.value(), std::memory_order_relaxed);
            }
            Result<int> b = co_await co_async([]() -> int { return 5; });
            if (b) {
                acc->fetch_add(b.value(), std::memory_order_relaxed);
            }
            done->store(true);
            co_return;
        };
        launch(seq());
        wait_until(*done, 1000ms);
        AURORA_TEST_CHECK(done->load());
        AURORA_TEST_CHECK(acc->load() == 15);
    }
}
