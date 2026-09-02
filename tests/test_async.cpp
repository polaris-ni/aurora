// test_async.cpp — 覆盖回调式 au::async().then()：成功/错误/取消/超时（向后兼容，无 poster 直接调用）。

#include <atomic>
#include <chrono>
#include <thread>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::async;
using aurora::Result;

using std::chrono_literals::operator""ms;

namespace {
void wait_until(const std::atomic<bool> &flag, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!flag.load() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(5ms);
    }
}
} // namespace

AURORA_TEST() {
    // 1) 成功路径：无 poster 时 then() 回调直接调用。
    {
        std::atomic done{ false };
        Result got{ 0 };
        async([]() -> int { return 21 * 2; }).then([&](const Result<int> &r) -> void {
            got = r;
            done = true;
        });
        wait_until(done, 1000ms);
        AURORA_TEST_CHECK(done.load());
        AURORA_TEST_CHECK(got.ok());
        AURORA_TEST_CHECK(got.value() == 42);
    }

    // 2) fn 返回 Result<T>：透传成功值。
    {
        std::atomic done{ false };
        Result got{ 0 };
        async([]() -> Result<int> { return Result{ 99 }; }).then([&](const Result<int> &r) -> void {
            got = r;
            done = true;
        });
        wait_until(done, 1000ms);
        AURORA_TEST_CHECK(done.load());
        AURORA_TEST_CHECK(got.ok() && got.value() == 99);
    }

    // 3) 异常被捕获为 async-exception 错误（不跨线程抛出）。
    {
        std::atomic done{ false };
        Result got{ 0 };
        async([]() -> int { throw std::runtime_error("fail"); }).then([&](const Result<int> &r) -> void {
            got = r;
            done = true;
        });
        wait_until(done, 1000ms);
        AURORA_TEST_CHECK(done.load());
        AURORA_TEST_CHECK(!got.ok());
        AURORA_TEST_CHECK(got.error().code == "runtime-async-exception");
    }

    // 4) 取消：then() 回调不被调用（后台仍跑完，仅丢弃结果）。
    {
        std::atomic called{ false };
        auto task = async([]() -> int {
            std::this_thread::sleep_for(80ms);
            return 1;
        });
        task.cancel();
        task.then([&](const Result<int> &) -> void { called = true; });
        std::this_thread::sleep_for(300ms); // 等待后台（已被取消）跑完
        AURORA_TEST_CHECK(!called.load());
        AURORA_TEST_CHECK(task.is_cancelled());
    }

    // 5) 超时：短超时 + 长任务 → 回调收到 async-timeout 错误。
    {
        std::atomic done{ false };
        Result got{ 0 };
        async([]() -> int {
            std::this_thread::sleep_for(200ms);
            return 1;
        })
            .with_timeout(20ms)
            .then([&](const Result<int> &r) -> void {
                got = r;
                done = true;
            });
        wait_until(done, 1000ms);
        AURORA_TEST_CHECK(done.load());
        AURORA_TEST_CHECK(!got.ok());
        AURORA_TEST_CHECK(got.error().code == "async-timeout");
    }

    // 6) 超时未到：正常完成，无超时错误。
    {
        std::atomic done{ false };
        Result got{ 0 };
        async([]() -> int { return 7; }).with_timeout(500ms).then([&](const Result<int> &r) -> void {
            got = r;
            done = true;
        });
        wait_until(done, 1000ms);
        AURORA_TEST_CHECK(done.load());
        AURORA_TEST_CHECK(got.ok() && got.value() == 7);
    }
}
