// test_thread_pool.cpp — 覆盖公开 aurora::ThreadPool（有界 worker / 默认池 / 任务完成 / 析构 join）。
#include <atomic>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::ThreadPool;

AURORA_TEST() {
    // 1) 并发提交：所有任务都执行完毕。
    {
        ThreadPool pool(4);
        std::atomic counter{0};
        std::vector<std::future<void>> futs;
        futs.reserve(200);
        for (int i = 0; i < 200; ++i) {
            futs.push_back(pool.submit([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));
        }
        for (auto &f : futs) {
            f.get();  // 阻塞直到该任务完成（异常经 future 传播）
        }
        AURORA_TEST_CHECK(counter.load() == 200);
    }

    // 2) worker 数有界（>=2）。
    {
        ThreadPool pool(3);
        AURORA_TEST_CHECK(pool.worker_count() == 3);
    }
    {
        ThreadPool def;
        AURORA_TEST_CHECK(def.worker_count() >= 2);
    }

    // 3) 默认池为进程级单例（多次调用同一实例）。
    {
        ThreadPool &a = ThreadPool::default_pool();
        ThreadPool &b = ThreadPool::default_pool();
        AURORA_TEST_CHECK(&a == &b);
    }

    // 4) 返回值经 future 正确传递。
    {
        ThreadPool pool(2);
        auto f = pool.submit([]() -> int { return 7 * 6; });
        AURORA_TEST_CHECK(f.get() == 42);
    }

    // 5) 异常经 future 传播（不跨 worker 抛出）。
    {
        ThreadPool pool(2);
        auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
        bool threw = false;
        try {
            f.get();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        AURORA_TEST_CHECK(threw);
    }

    // 6) 析构前排队任务全部执行（join 保证）。
    {
        std::atomic counter{0};
        {
            ThreadPool pool(2);
            for (int i = 0; i < 50; ++i) {
                pool.execute([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
            }
            // pool 析构时 join 所有 worker，保证 50 个任务执行完。
        }
        AURORA_TEST_CHECK(counter.load() == 50);
    }
}
