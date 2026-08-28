#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/core/thread_pool.h"

namespace aurora {

namespace detail {

/// @brief `async` 内部共享状态（在 `Task` 与后台 job 间共享，生命周期由 `shared_ptr` 管理）。
template<typename T> struct AsyncState {
    std::mutex m_mutex;
    std::function<void(const Result<T> &)> m_on_done;
    std::optional<Result<T>> m_result; ///< 后台计算完成后的结果（未就绪则为空）
    std::atomic<bool> m_cancelled{ false };
    bool m_delivered{ false }; ///< 已向回调投递（成功/取消/超时）避免重复
};

// 主线程投递器（进程级单例存储）。
inline auto main_poster_mutex() -> auto & {
    static std::mutex m;
    return m;
}
inline auto main_poster() -> auto & {
    static std::function<void(std::function<void()>)> p;
    return p;
}

/// @brief 经主线程投递器执行 `fn`；无 poster 时直接调用（headless / 测试）。
inline auto post_to_main(std::function<void()> fn) -> void {
    std::function<void(std::function<void()>)> poster;
    {
        std::scoped_lock lock(main_poster_mutex());
        poster = main_poster();
    }
    if (poster) {
        poster(std::move(fn));
    } else {
        fn(); // 无事件循环：直接调用（测试 / 无头场景）
    }
}

/// @brief 若可投递（结果就绪 + 已注册回调 + 未取消/未投递），复制出并标记 delivered；
/// 返回待投递结果（不可投递则返回 nullopt）。`cb` 取出已注册回调。
template<typename T>
auto take_for_delivery(AsyncState<T> &st, std::function<void(const Result<T> &)> &cb) -> std::optional<Result<T>> {
    std::scoped_lock<std::mutex> lock(st.m_mutex);
    if (st.m_delivered) {
        return std::nullopt;
    }
    if (!st.m_result.has_value()) {
        return std::nullopt; // 后台尚未就绪，等 worker 完成后再 deliver
    }
    if (st.m_cancelled) {
        st.m_delivered = true; // 已取消：丢弃，不调用回调
        return std::nullopt;
    }
    if (!st.m_on_done) {
        return std::nullopt; // 回调未注册：保持 ready，待 then() 注册后补投
    }
    cb = st.m_on_done;
    st.m_delivered = true;
    return st.m_result; // 复制 Result<T>（此时必有值）
}

// 萃取 fn 的返回类型：若为 Result<U> 则任务值为 U，否则为 Raw。
template<typename R> struct TaskValueOf {
    using type = R;
};
template<typename U> struct TaskValueOf<Result<U>> {
    using type = U;
};
template<typename R> using TaskValueOfT = TaskValueOf<R>::type;

/// @brief 安全调用 fn：返回 Result<ValueT>，异常捕获为 make_error(ErrorCode::RuntimeAsyncException, ...)（slug 为
/// `"runtime-async-exception"`）。
template<typename F> auto invoke_safe(F &&f) {
    using Raw = std::invoke_result_t<F>;
    using ValueT = TaskValueOfT<Raw>;
    try {
        if constexpr (std::is_same_v<Raw, Result<ValueT>>) {
            return std::forward<F>(f)(); // fn 已返回 Result<ValueT>
        } else {
            return Result<ValueT>{ std::forward<F>(f)() }; // fn 返回裸 ValueT
        }
    } catch (const std::exception &e) {
        return Result<ValueT>{
            make_error(ErrorCode::RuntimeAsyncException, std::string("async task threw: ") + e.what()),
        };
    } catch (...) {
        return Result<ValueT>{ make_error(ErrorCode::RuntimeAsyncException, "async task threw unknown exception") };
    }
}

} // namespace detail

/**
 * @brief 轻量异步任务（需求 #19 / 规格 §3.5 / §H.10）。
 *
 * 在单线程 UI 约束下，把「后台计算」与「主线程结果回写」解耦：
 * - `async(fn)` 立即返回一个 `Task<T>`，在**有界线程池**（`ThreadPool::default_pool()`）
 *   的后台 worker 执行 `fn`；不再为每次调用 `std::thread().detach()` 起 OS 线程。
 * - 结果就绪后，经 `then(onDone)` 把回调调度回「主线程」（通过主线程投递器），
 *   可回写 `State<T>` 以触发响应式定点刷新（与现有信号系统无缝衔接）。
 *
 * 取消语义：`cancel()` 标记任务为已取消，后台线程仍会执行完毕（无法中断任意函数），
 * 但 `then` 回调不会被调用。适用于「不再关心结果」的场景。
 *
 * 超时语义：`with_timeout(d)` 注册一个超时看守（经线程池提交，非 detached）；
 * 若 `d` 内任务未 `deliver`，则向 `then` 回调投递 `make_error(ErrorCode::RuntimeAsyncTimeout, ...)`（slug 为
 * `"async-timeout"`）。 与 `cancel` 同限制——无法中断任意 `fn`，仅丢弃/改道结果。
 *
 * 因 UI 为单线程，默认直接在主线程调用 `then` 回调（无内部跨线程派发）；
 * 真实事件循环可调用 `Task<T>::set_main_poster` 把回调投入主线程队列，避免跨线程访问 widget。
 * @note Thread: thread-safe with mutex
 * @note Side-effects: none
 */
template<typename T> class Task {
  public:
    using DoneFn = std::function<void(const Result<T> &)>;

    explicit Task(std::shared_ptr<detail::AsyncState<T>> state) : m_state(std::move(state)) {}

    /// @brief 注册完成回调（结果经主线程投递器回调）。返回自身以便链式。
    auto then(DoneFn cb) -> Task & {
        std::function<void(const Result<T> &)> replay_cb;
        std::optional<Result<T>> replay_r;
        bool has_replay = false;
        {
            std::scoped_lock<std::mutex> lock(m_state->m_mutex);
            m_state->m_on_done = std::move(cb);
            // 若结果此前已 deliver（成功/超时/取消）但当时 on_done 为空，则补投到新回调。
            if (m_state->m_delivered && !m_state->m_cancelled && m_state->m_result.has_value()) {
                replay_cb = m_state->m_on_done;
                replay_r = m_state->m_result;
                has_replay = true;
            }
        }
        if (has_replay) {
            auto r = std::move(*replay_r);
            detail::post_to_main([replay_cb, r]() mutable -> void { replay_cb(r); });
        } else {
            try_deliver();
        }
        return *this;
    }

    /// @brief 取消任务：标记为已取消，`then` 回调不会被调用。
    /// 注意：后台线程仍会执行完毕（无法中断任意函数），仅丢弃结果。
    auto cancel() -> void {
        std::scoped_lock<std::mutex> lock(m_state->m_mutex);
        m_state->m_cancelled.store(true, std::memory_order_release);
        m_state->m_delivered = true; // 取消即视为已处理，丢弃后续回调
    }

    /// @brief 查询是否已取消。
    [[nodiscard]] auto is_cancelled() const -> bool { return m_state->m_cancelled.load(std::memory_order_acquire); }

    /// @brief 注册超时：超过 `d` 任务仍未回写，则向 `then` 回调投递 `async-timeout` 错误。
    /// 返回自身以便链式。仅 opt-in 时占用一个池任务（非 detached 线程）。
    template<typename Rep, typename Period> auto with_timeout(std::chrono::duration<Rep, Period> d) -> Task & {
        auto state = m_state;
        ThreadPool::default_pool().execute([state, d]() -> void {
            std::this_thread::sleep_for(d);
            std::function<void(const Result<T> &)> to_call;
            std::optional<Result<T>> err;
            {
                std::scoped_lock<std::mutex> lock(state->m_mutex);
                if (state->m_delivered || state->m_cancelled) {
                    return; // 已完成或已取消，超时无效
                }
                state->m_delivered = true;
                state->m_result = make_error(ErrorCode::RuntimeAsyncTimeout, "async task timed out");
                err = state->m_result;
                to_call = state->m_on_done;
            }
            if (to_call && err) {
                auto r = std::move(*err);
                detail::post_to_main([to_call, r]() mutable -> void { to_call(r); });
            }
        });
        return *this;
    }

    /// @brief 设置主线程投递器（事件循环调用；默认直接调用）。线程安全。
    static auto set_main_poster(std::function<void(std::function<void()>)> poster) -> void {
        std::scoped_lock lock(detail::main_poster_mutex());
        detail::main_poster() = std::move(poster);
    }

  private:
    /// @brief 若结果已就绪且已注册回调，则投递（去重：仅首次 deliver 调用）。
    auto try_deliver() -> void {
        std::function<void(const Result<T> &)> to_call;
        auto maybe = detail::take_for_delivery(*m_state, to_call);
        if (maybe) {
            auto r = std::move(*maybe);
            detail::post_to_main([to_call, r]() mutable -> void { to_call(r); });
        }
    }

    std::shared_ptr<detail::AsyncState<T>> m_state;
};

/// @brief 启动异步任务：在后台线程池执行 `fn`（返回 `T` 或 `Result<T>`），返回 `Task<ValueT>`。
/// 返回的 Task 支持 `then()` 回主线程、`cancel()` 取消、`with_timeout()` 超时。
template<typename F> auto async(F &&fn) {
    using ValueT = detail::TaskValueOfT<std::invoke_result_t<F>>;

    auto state = std::make_shared<detail::AsyncState<ValueT>>();
    auto state_capture = state;
    auto f = std::forward<F>(fn);

    ThreadPool::default_pool().execute([state = std::move(state_capture), f = std::move(f)]() mutable -> void {
        {
            Result<ValueT> r = detail::invoke_safe(std::move(f));
            std::scoped_lock<std::mutex> lock(state->m_mutex);
            state->m_result = std::move(r);
        }
        std::function<void(const Result<ValueT> &)> to_call;
        auto maybe = detail::take_for_delivery(*state, to_call);
        if (maybe) {
            auto out = std::move(*maybe);
            detail::post_to_main([to_call, out]() mutable -> void { to_call(out); });
        }
    });

    return Task<ValueT>{ std::move(state) };
}

} // namespace aurora
