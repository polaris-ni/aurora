#pragma once

#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace aurora {

/**
 * @brief 有界 worker 线程池（需求 #19 / specification/01-core.md §6.1 异步层基础设施）。
 *
 * 取代「每次 `std::thread().detach()` 直接起 OS 线程」的旧实现：所有后台工作提交到
 * 任务队列，固定数量 worker 线程复用消费。线程数有界（`default_worker_count()`，
 * 默认 `hardware_concurrency()`，下限 2），避免突发 `async` 调用的线程爆炸；
 * 析构时 `stop()` + `join()` 全部 worker，**无 detached 悬挂线程**，进程退出安全。
 *
 * 用法：
 * @code
 *   au::ThreadPool::default_pool().execute([] { background_work(); });
 *   auto fut = au::ThreadPool::default_pool().submit([] { return compute(); });
 *   // fut.get() 取结果（异常经 future 传播）
 * @endcode
 *
 * 线程安全：所有公开方法可并发调用。
 */
class ThreadPool {
  public:
    /// @brief 默认 worker 数：`hardware_concurrency()`，下限 2（单核/查询失败时为 2）。
    [[nodiscard]] static auto default_worker_count() -> std::size_t {
        const unsigned hc = std::thread::hardware_concurrency();
        if (hc < 2U) {
            return 2U;
        }
        return hc;
    }

    /// @brief 构造并启动 `worker_count` 个 worker 线程。
    explicit ThreadPool(std::size_t worker_count = default_worker_count()) {
        if (worker_count == 0) {
            worker_count = default_worker_count();
        }
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() -> void { worker_loop(); });
        }
    }

    /// @brief 停止并 join 全部 worker（RAII 安全，无悬挂线程）。
    ~ThreadPool() {
        {
            std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread &w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    ThreadPool(const ThreadPool &) = delete;
    auto operator=(const ThreadPool &) -> ThreadPool & = delete;
    ThreadPool(ThreadPool &&) = delete;
    auto operator=(ThreadPool &&) -> ThreadPool & = delete;

    /// @brief 当前 worker 线程数。
    [[nodiscard]] auto worker_count() const -> std::size_t { return workers_.size(); }

    /// @brief 当前排队未执行的任务数（近似值，仅供诊断）。
    [[nodiscard]] auto pending_count() const -> std::size_t {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 提交一个 fire-and-forget 任务到队列。
     * @param job 可执行体（被拷贝/移动到队列中）。异常会在 worker 内被吞掉并经由
     *            `std::terminate` 之前的最后一道屏障——实际由 `execute` 包裹捕获，
     *            不向外传播；若需传播请用 `submit`（返回 `std::future`）。
     */
    auto execute(std::function<void()> job) -> void {
        {
            std::scoped_lock lock(mutex_);
            // 契约（见上方注释）：fire-and-forget 任务的异常在此吞掉，绝不跨出 worker 线程。
            queue_.emplace([job = std::move(job)]() mutable -> void {
                try {
                    job();
                } catch (...) { // NOLINT(*-empty-catch)
                    // fire-and-forget：无处投递异常，吞掉即最后一道屏障
                }
            });
        }
        cv_.notify_one();
    }

    /**
     * @brief 提交一个任务并返回 `std::future<R>` 取结果/异常（异常经 future 传播）。
     * @tparam F 可调用体，返回 `R`（`void` 亦可）。
     */
    template <typename F>
    auto submit(F &&f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto promise = std::make_shared<std::promise<R>>();
        std::future<R> fut = promise->get_future();
        execute([promise, f = std::forward<F>(f)]() mutable -> void {
            try {
                if constexpr (std::is_same_v<R, void>) {
                    f();
                    promise->set_value();
                } else {
                    promise->set_value(f());
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        return fut;
    }

    /**
     * @brief 进程级默认线程池（Meyers 单例）：`au::async` / 协程均经它调度。
     * 跨 TU 单实例（C++17 inline 语义）；程序退出时静态析构 join 全部 worker。
     */
    [[nodiscard]] static auto default_pool() -> ThreadPool & {
        static ThreadPool instance(default_worker_count());
        return instance;
    }

  private:
    auto worker_loop() -> void {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() -> bool { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) {
                    return;
                }
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();  // 在 worker 线程执行；异常由 job 内部（execute 包裹）捕获，不跨出。
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

}  // namespace aurora
