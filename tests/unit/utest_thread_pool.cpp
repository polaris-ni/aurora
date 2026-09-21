/// 测试类型: unit
/// 目标单元: include/aurora/core/thread_pool.h
/// 测试说明: 线程池的默认 worker 数下限、构造线程数与零回退、submit 经 future 返回值/传播异常、void 任务与 execute
/// 完成通知、execute 吞异常保 worker 存活、pending_count 排队观测、default_pool 进程级单例（全部用 future +
/// 充裕超时等待，不做时序假设）；deferred 延迟排空模式（force_deferred，Emscripten 无 pthreads 的编译期形态）：
/// 入队不执行、pump 帧尾排空、预算不含重入队任务、异常吞掉、析构排空、非 deferred 池 pump 恒 0

#include <chrono>
#include <cstddef>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "aurora/core/thread_pool.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_thread_pool {

AURORA_TEST_CASE(default_worker_count_at_least_two) {
    // hardware_concurrency 查询失败/单核时下限为 2。
    AURORA_TEST_CHECK_GE(aurora::ThreadPool::default_worker_count(), std::size_t{2});
}

AURORA_TEST_CASE(deferred_pool_queues_until_pump) {
    // deferred：任务只入队，pump() 在当前线程执行；不启动任何 worker。
    aurora::ThreadPool pool{4, /*force_deferred=*/true};
    AURORA_TEST_CHECK_TRUE(pool.is_deferred());
    AURORA_TEST_CHECK_EQ(pool.worker_count(), std::size_t{0});

    int ran = 0;
    pool.execute([&ran]() -> void { ++ran; });
    AURORA_TEST_CHECK_EQ(ran, 0);
    AURORA_TEST_CHECK_EQ(pool.pending_count(), std::size_t{1});

    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(ran, 1);
    AURORA_TEST_CHECK_EQ(pool.pending_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{0});  // 空队列：无事可做
}

AURORA_TEST_CASE(deferred_submit_resolves_via_pump) {
    aurora::ThreadPool pool{2, /*force_deferred=*/true};
    auto fut = pool.submit([]() -> int { return 21 * 2; });
    AURORA_TEST_CHECK_EQ(pool.pending_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 42);

    // 异常经 promise 走同一通道：pump 后由 future 携带。
    auto bad = pool.submit([]() -> int { throw std::runtime_error{"deferred fail"}; });
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{1});
    bool threw = false;
    try {
        (void)bad.get();
    } catch (const std::runtime_error& e) {
        threw = true;
        AURORA_TEST_CHECK_STREQ(e.what(), "deferred fail");
    }
    AURORA_TEST_CHECK_TRUE(threw);
}

AURORA_TEST_CASE(deferred_pump_budget_excludes_requeued_jobs) {
    // 契约：pump 只执行「进入时已入队」的任务——自我续命任务留待下一次，饿不死宿主帧。
    aurora::ThreadPool pool{2, /*force_deferred=*/true};
    int generations = 0;
    pool.execute([&pool, &generations]() -> void {
        ++generations;
        pool.execute([&pool, &generations]() -> void {  // 帧内向本池再投一单
            ++generations;
            (void)pool;
        });
    });
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{1});  // 本轮只跑第 1 代
    AURORA_TEST_CHECK_EQ(generations, 1);
    AURORA_TEST_CHECK_EQ(pool.pending_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{1});  // 第 2 代在下一轮
    AURORA_TEST_CHECK_EQ(generations, 2);
}

AURORA_TEST_CASE(deferred_execute_swallows_exceptions) {
    // 与 worker 模式的 execute 契约一致：异常在任务包裹层吞掉，后续任务照常排空。
    aurora::ThreadPool pool{2, /*force_deferred=*/true};
    int after = 0;
    pool.execute([]() -> void { throw std::runtime_error{"boom"}; });
    pool.execute([&after]() -> void { ++after; });
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(after, 1);
}

AURORA_TEST_CASE(deferred_destructor_drains_pending_queue) {
    // 析构语义对齐 worker 池（stop 前排空）：未 pump 的任务在析构时执行，不静默丢弃。
    int ran = 0;
    {
        aurora::ThreadPool pool{2, /*force_deferred=*/true};
        pool.execute([&ran]() -> void { ++ran; });
        AURORA_TEST_CHECK_EQ(ran, 0);
    }
    AURORA_TEST_CHECK_EQ(ran, 1);
}

AURORA_TEST_CASE(worker_pool_pump_is_noop) {
    AURORA_TEST_REQUIRE_THREADS();
    // 非 deferred 池 pump() 恒 0——排空归 worker 线程，宿主无参与。
    aurora::ThreadPool pool{2};
    AURORA_TEST_REQUIRE_TRUE(!pool.is_deferred());
    int ran = 0;
    pool.execute([&ran]() -> void { ++ran; });
    AURORA_TEST_CHECK_EQ(pool.pump(), std::size_t{0});
}

AURORA_TEST_CASE(constructor_scales_workers_and_rejects_zero) {
    AURORA_TEST_REQUIRE_THREADS();
    const aurora::ThreadPool pool{3};
    AURORA_TEST_CHECK_EQ(pool.worker_count(), std::size_t{3});

    // 0 视为非法 worker 数：回退到 default_worker_count()。
    const aurora::ThreadPool fallback{0};
    AURORA_TEST_CHECK_EQ(fallback.worker_count(), aurora::ThreadPool::default_worker_count());
}

AURORA_TEST_CASE(submit_returns_value_via_future) {
    AURORA_TEST_REQUIRE_THREADS();
    aurora::ThreadPool pool{2};
    auto fut = pool.submit([]() -> int { return 21 * 2; });
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 42);
}

AURORA_TEST_CASE(submit_propagates_exceptions_through_future) {
    AURORA_TEST_REQUIRE_THREADS();
    aurora::ThreadPool pool{2};
    auto fut = pool.submit([]() -> int { throw std::runtime_error{"task failed"}; });
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    bool threw = false;
    try {
        (void)fut.get();
    } catch (const std::runtime_error& e) {
        threw = true;
        AURORA_TEST_CHECK_STREQ(e.what(), "task failed");
    }
    AURORA_TEST_CHECK_TRUE(threw);
}

AURORA_TEST_CASE(void_submit_and_execute_complete_deterministically) {
    AURORA_TEST_REQUIRE_THREADS();
    aurora::ThreadPool pool{2};

    // submit 的 void 形态：future 就绪即任务已完成。
    bool ran = false;
    const auto fut = pool.submit([&ran]() -> void { ran = true; });
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_TRUE(ran);

    // execute 为 fire-and-forget：经 promise 观测完成。
    std::promise<void> done;
    const auto done_fut = done.get_future();
    pool.execute([&done]() -> void { done.set_value(); });
    AURORA_TEST_CHECK_EQ(done_fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
}

AURORA_TEST_CASE(execute_swallows_exceptions_fire_and_forget) {
    AURORA_TEST_REQUIRE_THREADS();
    // 契约：execute 任务的异常在 worker 内吞掉，绝不跨出 worker 线程。
    aurora::ThreadPool pool{1};  // 单 worker 保证 FIFO 顺序
    std::promise<int> survived;
    auto fut = survived.get_future();
    pool.execute([]() -> void { throw std::runtime_error{"fire and forget"}; });
    pool.execute([&survived]() -> void { survived.set_value(1); });
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 1);  // 后续任务仍被消费：worker 未被异常杀死
}

AURORA_TEST_CASE(pending_count_reflects_queued_work) {
    AURORA_TEST_REQUIRE_THREADS();
    aurora::ThreadPool pool{1};
    std::promise<void> release;
    const auto gate = release.get_future().share();  // 多个任务共享等待同一闸门
    std::vector<std::future<void>> running;
    running.reserve(4);
    for (int i = 0; i < 4; ++i) {
        running.push_back(pool.submit([gate]() -> void { gate.wait(); }));
    }
    // 截止时间轮询（最多 5s、每 10ms 一次）：单 worker 执行 1 个，其余 3 个排队。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline && pool.pending_count() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    AURORA_TEST_CHECK_GE(pool.pending_count(), std::size_t{3});

    // 放闸后全部任务完成，队列清空。
    release.set_value();
    for (auto& f : running) {
        AURORA_TEST_CHECK_EQ(f.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    }
    AURORA_TEST_CHECK_EQ(pool.pending_count(), std::size_t{0});
}

AURORA_TEST_CASE(default_pool_is_process_wide_singleton) {
    AURORA_TEST_REQUIRE_THREADS();
    // Meyers 单例：跨调用同址。
    AURORA_TEST_CHECK(&aurora::ThreadPool::default_pool() == &aurora::ThreadPool::default_pool());
    auto fut = aurora::ThreadPool::default_pool().submit([]() -> int { return 40 + 2; });
    AURORA_TEST_CHECK_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready);
    AURORA_TEST_CHECK_EQ(fut.get(), 42);
}

}  // namespace aurora::test_cases::utest_thread_pool
