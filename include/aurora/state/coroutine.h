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
 *     if (r) store->dispatch(au::Action{"loaded", r.value()});
 *     else   Diagnostics::report(r.error().message, {}, r.error().code);
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
    // `promise_type` 是 `std::coroutine_traits` 固定查找的类型名（改名即协程语法失效），非本库命名自由度；
    // 协程钩子三件套亦由协议规定，故与 special-member-functions 一并就地豁免。
    // NOLINTNEXTLINE(*-special-member-functions,readability-identifier-naming)
    struct promise_type {
        std::shared_ptr<detail::CoroShared<T>> shared = std::make_shared<detail::CoroShared<T>>();

        auto get_return_object() -> CoroTask { return CoroTask{shared}; }
        // 协程协议钩子由标准固定为「在 promise 对象上调用」，非本库设计自由度；
        // 同体内 return_value/unhandled_exception 必须访问 shared，故三件套统一保持实例方法。
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto initial_suspend() -> std::suspend_never { return {}; }  // 立即开始执行
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto final_suspend() noexcept -> std::suspend_never { return {}; }  // 结束即销毁帧
        auto return_value(T v) -> void { shared->result = Result<T>{std::move(v)}; }
        auto unhandled_exception() -> void {
            try {
                throw;
            } catch (const std::exception &e) {
                shared->result =
                    make_error(ErrorCode::RuntimeCoroutineException, std::string("coroutine threw: ") + e.what());
            } catch (...) {
                shared->result = make_error(ErrorCode::RuntimeCoroutineException, "coroutine threw unknown exception");
            }
        }
        ~promise_type() { shared->done.store(true, std::memory_order_release); }
    };

    explicit CoroTask(std::shared_ptr<detail::CoroShared<T>> shared) : shared_(std::move(shared)) {}

    /// @brief 协程是否已完成（含异常）。
    [[nodiscard]] auto is_done() const -> bool { return shared_->done.load(std::memory_order_acquire); }

    /// @brief 协程返回值（仅非 void；异常时返回错误 Result；未完成前调用结果未定义）。
    // 豁免本检查：契约要求先 `is_done()`。`return_value` 与 `unhandled_exception` 是协程收尾的必经
    // 两条路径、都写入 `result`，而 `done` 只在 `~promise_type` 置位——故 `done` 为真时 `result`
    // 必有值。要改成运行期分支需新增「未完成」公共错误码（现无契合项，属 API 扩张），且不改本检查
    // 建议的形态；调用方违约即文档化的未定义行为。同一份代码 native 遍不报（口径差异见
    // CODING_STANDARDS.md §5.2）。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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
    struct promise_type {  // NOLINT(*-special-member-functions,readability-identifier-naming) 同上：协程固定类型名
        std::shared_ptr<detail::CoroShared<void>> shared = std::make_shared<detail::CoroShared<void>>();

        [[nodiscard]] auto get_return_object() const -> CoroTask { return CoroTask{shared}; }
        // 协程协议钩子：调用形态由标准固定，与 CoroTask<T> 主模板保持一致，勿改 static。
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto initial_suspend() -> std::suspend_never { return {}; }
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto final_suspend() noexcept -> std::suspend_never { return {}; }
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto return_void() -> void {}
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

    // awaiter 三件套：await_suspend/await_resume 必须访问 this，await_ready 保持实例形态以统一调用方式。
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] auto await_ready() const -> bool { return false; }  // 始终挂起，交线程池执行

    auto await_suspend(std::coroutine_handle<> handle) -> void {
        // 把 fn 投入线程池；完成后经主线程投递器恢复续体（无 poster 时由 worker 直接 resume）。
        ThreadPool::default_pool().execute([this, handle]() mutable -> void {
            value_ = detail::invoke_safe(std::move(f_));
            detail::post_to_main([handle]() mutable -> void { handle.resume(); });
        });
    }

    // 豁免本检查：`value_` 由 `await_suspend` 投入线程池的任务写入，且写入先于同一任务内的
    // `handle.resume()`——续体只可能经那条 resume 前进，故 `await_resume` 取用时必有值。分析器看
    // 不到这条跨线程 happens-before，只能按「可能为空」报；在此加分支等于把协程契约撕成运行期
    // 判断，无对应错误码可用。同一份代码 native 遍不报（口径差异见 CODING_STANDARDS.md §5.2）。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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
