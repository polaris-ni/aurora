#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "aurora/state/async.h"

namespace aurora {

namespace detail {
// 协程共享完成状态：在 promise 与 launch 返回的句柄间共享生命周期。
template <typename T>
struct CoroShared {
    std::atomic<bool> done{false};
    std::optional<Result<T>> result;
};
template <>
struct CoroShared<void> {
    std::atomic<bool> done{false};
    std::optional<Error> error;
};
}  // namespace detail

/**
 * @brief 协程式异步任务返回类型（需求 #19 / specification/02-state.md §5.2 协程路径）。
 *
 * 与回调式 `au::async().then()` 并存：`co_await au::co_async(fn)` 在后台线程池执行 `fn`，
 * 续体（coroutine 后续代码）经主线程投递器恢复到主线程（无 poster 时由 worker 直接 resume，
 * 语义等同回调的直接调用）；`fn` 返回 `T` 或 `Result<T>`，`co_await` 表达式求得 `Result<ValueT>`。
 *
 * 典型用法：
 * @code
 *   au::CoroTask<void> load() {
 *     au::Result<Data> r = co_await au::co_async([] { return fetch(); });
 *     if (r) store->set(r.value());
 *     else   Diagnostics::error(r.error().message);
 *   }
 *   au::launch(load());
 * @endcode
 *
 * @note Thread: main-thread only (continuation resumes on main thread)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename T>
class CoroTask {
  public:
    // NOLINTNEXTLINE
    struct promise_type {
        std::shared_ptr<detail::CoroShared<T>> shared = std::make_shared<detail::CoroShared<T>>();

        auto get_return_object() -> CoroTask { return CoroTask{shared}; }
        // NOLINTNEXTLINE
        auto initial_suspend() -> std::suspend_never { return {}; }  // 立即开始执行
        // NOLINTNEXTLINE
        auto final_suspend() noexcept -> std::suspend_never { return {}; }  // 结束即销毁帧
        auto return_value(T v) -> void { shared->result = Result<T>{std::move(v)}; }
        auto unhandled_exception() -> void {
            try {
                throw;
            } catch (const std::exception &e) {
                shared->result =
                    make_error(ErrorCode::RuntimeCoroutineException, std::string("coroutine threw: ") + e.what());
            } catch (...) {
                shared->result =
                    make_error(ErrorCode::RuntimeCoroutineException, "coroutine threw unknown exception");
            }
        }
        ~promise_type() { shared->done.store(true, std::memory_order_release); }
    };

    explicit CoroTask(std::shared_ptr<detail::CoroShared<T>> shared) : shared_(std::move(shared)) {}

    /// @brief 协程是否已完成（含异常）。
    [[nodiscard]] auto is_done() const -> bool { return shared_->done.load(std::memory_order_acquire); }

    /// @brief 协程返回值（仅非 void；未完成/异常时为错误 Result）。
    [[nodiscard]] auto result() const -> Result<T> { return *shared_->result; }

  private:
    std::shared_ptr<detail::CoroShared<T>> shared_;
};

/// @brief `void` 特化：无返回值，续体仍回主线程。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <>
class CoroTask<void> {
  public:
    struct promise_type {  // NOLINT(*-special-member-functions)
        std::shared_ptr<detail::CoroShared<void>> shared = std::make_shared<detail::CoroShared<void>>();

        [[nodiscard]] auto get_return_object() const -> CoroTask { return CoroTask{shared}; }
        static auto initial_suspend() -> std::suspend_never { return {}; }
        static auto final_suspend() noexcept -> std::suspend_never { return {}; }
        static auto return_void() -> void {}
        auto unhandled_exception() const -> void {
            try {
                throw;
            } catch (const std::exception &e) {
                shared->error =
                    make_error(ErrorCode::RuntimeCoroutineException, std::string("coroutine threw: ") + e.what());
            } catch (...) {
                shared->error = make_error(ErrorCode::RuntimeCoroutineException, "coroutine threw unknown exception");
            }
        }
        ~promise_type() { shared->done.store(true, std::memory_order_release); }
    };

    explicit CoroTask(std::shared_ptr<detail::CoroShared<void>> shared) : shared_(std::move(shared)) {}

    [[nodiscard]] auto is_done() const -> bool { return shared_->done.load(std::memory_order_acquire); }
    [[nodiscard]] auto error() const -> std::optional<Error> { return shared_->error; }

  private:
    std::shared_ptr<detail::CoroShared<void>> shared_;
};

/**
 * @brief 协程等待体：`co_await co_async(fn)` 在后台线程池执行 `fn`，完成后恢复续体。
 * @tparam F 可调用体，返回 `T` 或 `Result<T>`。
 *
 * @note Thread: thread-safe (await_suspend posts to thread pool)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename F>
struct CoAwaitable {
    using ValueT = detail::TaskValueOfT<std::invoke_result_t<F>>;

    explicit CoAwaitable(F f) : f_(std::move(f)) {}

    [[nodiscard]] static auto await_ready() -> bool { return false; }  // 始终挂起，交线程池执行

    auto await_suspend(std::coroutine_handle<> handle) -> void {
        // 把 fn 投入线程池；完成后经主线程投递器恢复续体（无 poster 时由 worker 直接 resume）。
        ThreadPool::default_pool().execute([this, handle]() mutable -> void {
            value_ = detail::invoke_safe(std::move(f_));
            detail::post_to_main([handle]() mutable -> void { handle.resume(); });
        });
    }

    auto await_resume() -> Result<ValueT> { return std::move(*value_); }

  private:
    F f_;
    std::optional<Result<ValueT>> value_;
};

/// @brief 创建协程等待体：在后台线程池执行 `fn`，`co_await` 求得 `Result<ValueT>`。
template <typename F>
auto co_async(F &&f) -> CoAwaitable<std::decay_t<F>> {
    return CoAwaitable<std::decay_t<F>>(std::forward<F>(f));
}

/// @brief 启动顶层协程（fire-and-forget）。返回句柄可查询 `is_done()` / `result()`。
/// 协程已在调用 `coro()` 时开始执行（initial_suspend = never），`launch` 仅持有句柄保活。
template <typename T>
auto launch(CoroTask<T> task) -> CoroTask<T> {
    return task;
}
template <>
inline auto launch(CoroTask<void> task) -> CoroTask<void> {
    return task;
}

}  // namespace aurora
