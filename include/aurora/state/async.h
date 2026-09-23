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
#include <vector>

#include "aurora/core/result.h"
#include "aurora/core/thread_pool.h"

namespace aurora {

namespace detail {

/// @brief `async` 内部共享状态（在 `Task` 与后台 job 间共享，生命周期由 `shared_ptr` 管理）。
template <typename T>
struct AsyncState {
    std::mutex mutex;
    std::function<void(const Result<T> &)> on_done;
    std::optional<Result<T>> result;  ///< 后台计算完成后的结果（未就绪则为空）
    std::atomic<bool> cancelled{false};
    bool delivered{false};  ///< 已向回调投递（成功/取消/超时）避免重复
};

// 主线程投递器（进程级单例存储）。
inline auto main_poster_mutex() -> auto & {
    static std::mutex m;
    return m;
}
inline auto main_poster() -> auto & {
    // 惰性构造的函数内 static：首次调用才建，跨 TU 初始化顺序问题在此不存在（本检查的担心面）。
    // 仅浏览器口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
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
        fn();  // 无事件循环：直接调用（测试 / 无头场景）
    }
}

/// @brief 超时看守登记项：到期时刻 + 触发动作（类型擦除，动作自持对应 `AsyncState`）。
struct TimeoutGuard {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> expire;
};

// 看守登记表（进程级单例存储）：**仅** deferred 线程池构建使用——有 worker 时看守是
// 一个睡在后台线程上的池任务，无需主线程扫描。见 `Task<T>::with_timeout`。
inline auto timeout_guards_mutex() -> auto & {
    static std::mutex m;
    return m;
}
inline auto timeout_guards() -> std::vector<TimeoutGuard> & {
    static std::vector<TimeoutGuard> v;
    return v;
}

/// @brief 登记一个到期看守（`with_timeout` 的 deferred 分支）。条目活到自己到期为止：
/// 任务提前完成时它照常到期，只是触发时对 `delivered` 短路成 no-op，故表长上界 = 窗口 `d`。
inline auto register_timeout_guard(std::chrono::steady_clock::time_point deadline, std::function<void()> expire)
    -> void {
    std::scoped_lock lock(timeout_guards_mutex());
    timeout_guards().push_back(TimeoutGuard{.deadline = deadline, .expire = std::move(expire)});
}

/// @brief 最近登记的到期时刻距 `now` 的毫秒数；表空返回 `-1`（= 无看守，不参与唤醒决策）。
[[nodiscard]] inline auto next_timeout_deadline_ms(std::chrono::steady_clock::time_point now) -> double {
    std::scoped_lock lock(timeout_guards_mutex());
    double nearest = -1.0;
    for (const auto &g : timeout_guards()) {
        const double ms = std::chrono::duration<double, std::milli>(g.deadline - now).count();
        if (nearest < 0.0 || ms < nearest) {
            nearest = ms < 0.0 ? 0.0 : ms;  // 已到期：钳 0，催本帧立即扫描
        }
    }
    return nearest;
}

/**
 * @brief 帧尾扫描：摘出全部到期看守并在锁外逐个触发。
 * @return 本轮触发的看守数。
 *
 * 必须由宿主在**主线程安全点**每帧调用（`Application::step_frame()` 帧尾），且与
 * `ThreadPool::pump()` 同处一帧——deferred 构建下这是超时唯一能生效的地方：主线程不睡
 * 在看守任务里，也不会被看守任务阻塞。锁外触发是因为 `expire` 会回投主线程并可能登记新项。
 */
inline auto sweep_due_timeouts(std::chrono::steady_clock::time_point now) -> std::size_t {
    std::vector<TimeoutGuard> due;
    {
        std::scoped_lock lock(timeout_guards_mutex());
        auto &guards = timeout_guards();
        for (auto it = guards.begin(); it != guards.end();) {  // NOLINT(*-loop-convert)：erase 返回后继
            if (it->deadline <= now) {
                due.push_back(std::move(*it));
                it = guards.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &g : due) {
        g.expire();
    }
    return due.size();
}

/// @brief 让超时生效：结果仍未投递且未取消时，写入 `async-timeout` 错误并经主线程投递器回调。
template <typename T>
auto expire_timeout(const std::shared_ptr<AsyncState<T>> &state) -> void {
    std::function<void(const Result<T> &)> to_call;
    std::optional<Result<T>> err;
    {
        std::scoped_lock<std::mutex> lock(state->mutex);
        if (state->delivered || state->cancelled) {
            return;  // 已完成或已取消，超时无效
        }
        state->delivered = true;
        state->result = make_error(ErrorCode::RuntimeAsyncTimeout, "async task timed out");
        err = state->result;
        to_call = state->on_done;
    }
    if (to_call && err) {
        auto r = std::move(*err);
        post_to_main([to_call, r]() mutable -> void { to_call(r); });
    }
}

/// @brief 若可投递（结果就绪 + 已注册回调 + 未取消/未投递），复制出并标记 delivered；
/// 返回待投递结果（不可投递则返回 nullopt）。`cb` 取出已注册回调。
template <typename T>
auto take_for_delivery(AsyncState<T> &st, std::function<void(const Result<T> &)> &cb) -> std::optional<Result<T>> {
    std::scoped_lock<std::mutex> lock(st.mutex);
    if (st.delivered) {
        return std::nullopt;
    }
    if (!st.result.has_value()) {
        return std::nullopt;  // 后台尚未就绪，等 worker 完成后再 deliver
    }
    if (st.cancelled) {
        st.delivered = true;  // 已取消：丢弃，不调用回调
        return std::nullopt;
    }
    if (!st.on_done) {
        return std::nullopt;  // 回调未注册：保持 ready，待 then() 注册后补投
    }
    cb = st.on_done;
    st.delivered = true;
    return st.result;  // 复制 Result<T>（此时必有值）
}

// 萃取 fn 的返回类型：若为 Result<U> 则任务值为 U，否则为 Raw。
template <typename R>
struct TaskValueOf {
    // 类型萃取的成员名 `X::type` 是标准库既定形态（与 std traits 组合时按此名查找），非本库命名自由度。
    // NOLINTNEXTLINE(readability-identifier-naming)
    using type = R;
};
template <typename U>
struct TaskValueOf<Result<U>> {
    // 同上：`trait::type` 由惯例固定。
    // NOLINTNEXTLINE(readability-identifier-naming)
    using type = U;
};
template <typename R>
using TaskValueOfT = TaskValueOf<R>::type;

/// @brief 安全调用 fn：返回 Result<ValueT>，异常捕获为 make_error(ErrorCode::RuntimeAsyncException, ...)（slug 为
/// `"runtime-async-exception"`）。
template <typename F>
auto invoke_safe(F &&f) {
    using Raw = std::invoke_result_t<F>;
    using ValueT = TaskValueOfT<Raw>;
    try {
        if constexpr (std::is_same_v<Raw, Result<ValueT>>) {
            return std::forward<F>(f)();  // fn 已返回 Result<ValueT>
        } else {
            return Result<ValueT>{std::forward<F>(f)()};  // fn 返回裸 ValueT
        }
    } catch (const std::exception &e) {
        return Result<ValueT>{
            make_error(ErrorCode::RuntimeAsyncException, std::string("async task threw: ") + e.what()),
        };
    } catch (...) {
        return Result<ValueT>{make_error(ErrorCode::RuntimeAsyncException, "async task threw unknown exception")};
    }
}

}  // namespace detail

/**
 * @brief 轻量异步任务（需求 #19 / specification/02-state.md §5.1）。
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
 * 超时语义：`with_timeout(d)` 注册一个超时看守（worker 池下是一个后台池任务，deferred 池下是
 * 一张到期登记表，由宿主帧尾扫描触发——见 `Task<T>::with_timeout`）；
 * 若 `d` 内任务未 `deliver`，则向 `then` 回调投递 `make_error(ErrorCode::RuntimeAsyncTimeout, ...)`（slug 为
 * `"async-timeout"`）。 与 `cancel` 同限制——无法中断任意 `fn`，仅丢弃/改道结果。
 *
 * 未安装主线程投递器时，`then` 回调在承接 worker 线程同步执行；安装 poster 后才保证回到主线程；
 * 真实事件循环可调用 `Task<T>::set_main_poster` 把回调投入主线程队列，避免跨线程访问 widget。
 * @note Thread: thread-safe with mutex
 * @note Side-effects: none
 */
template <typename T>
class Task {
  public:
    using DoneFn = std::function<void(const Result<T> &)>;

    explicit Task(std::shared_ptr<detail::AsyncState<T>> state) : state_(std::move(state)) {}

    /// @brief 注册完成回调（结果经主线程投递器回调）。返回自身以便链式。
    auto then(DoneFn cb) -> Task & {
        std::function<void(const Result<T> &)> replay_cb;
        std::optional<Result<T>> replay_r;
        bool has_replay = false;
        {
            std::scoped_lock<std::mutex> lock(state_->mutex);
            state_->on_done = std::move(cb);
            // 若结果此前已 deliver（成功/超时/取消）但当时 on_done 为空，则补投到新回调。
            if (state_->delivered && !state_->cancelled && state_->result.has_value()) {
                replay_cb = state_->on_done;
                replay_r = state_->result;
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
        std::scoped_lock<std::mutex> lock(state_->mutex);
        state_->cancelled.store(true, std::memory_order_release);
        state_->delivered = true;  // 取消即视为已处理，丢弃后续回调
    }

    /// @brief 查询是否已取消。
    [[nodiscard]] auto is_cancelled() const -> bool { return state_->cancelled.load(std::memory_order_acquire); }

    /// @brief 注册超时：超过 `d` 任务仍未回写，则向 `then` 回调投递 `async-timeout` 错误。
    /// 返回自身以便链式。
    ///
    /// 两条实现路径按线程池模式分流，语义一致（到点改道结果、不中断 `fn`）：
    /// - worker 池：看守是一个睡 `d` 的后台池任务（原有形态），到点自行投递，经主线程投递器回投。
    /// - **deferred 池**：无后台线程可睡——睡在池任务里等于睡在主线程泵上，且看守排在被看守
    ///   任务之后（同队 FIFO），永远不可能先跑。故只登记到期时刻，由帧尾 `sweep_due_timeouts`
    ///   扫描触发；等待中的宿主循环经 `next_timeout_deadline_ms` 把期限并入唤醒决策，不深睡过头。
    template <typename Rep, typename Period>
    auto with_timeout(std::chrono::duration<Rep, Period> d) -> Task & {
        auto state = state_;
        auto expire = [state]() -> void { detail::expire_timeout(state); };
        if (ThreadPool::default_pool().is_deferred()) {
            detail::register_timeout_guard(std::chrono::steady_clock::now() + d, std::move(expire));
            return *this;
        }
        ThreadPool::default_pool().execute([expire = std::move(expire), d]() -> void {
            std::this_thread::sleep_for(d);
            expire();
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
        auto maybe = detail::take_for_delivery(*state_, to_call);
        if (maybe) {
            auto r = std::move(*maybe);
            detail::post_to_main([to_call, r]() mutable -> void { to_call(r); });
        }
    }

    std::shared_ptr<detail::AsyncState<T>> state_;
};

/// @brief 启动异步任务：在后台线程池执行 `fn`（返回 `T` 或 `Result<T>`），返回 `Task<ValueT>`。
/// 返回的 Task 支持 `then()` 回主线程、`cancel()` 取消、`with_timeout()` 超时。
template <typename F>
auto async(F &&fn) {
    using ValueT = detail::TaskValueOfT<std::invoke_result_t<F>>;

    auto state = std::make_shared<detail::AsyncState<ValueT>>();
    auto state_capture = state;
    auto f = std::forward<F>(fn);

    // NOLINTNEXTLINE(bugprone-exception-escape) 回调由 invoke_safe/execute 内 try/catch 兜底，抛异常不会逃出线程边界
    ThreadPool::default_pool().execute([state = std::move(state_capture), f = std::move(f)]() mutable -> void {
        {
            Result<ValueT> r = detail::invoke_safe(std::move(f));
            std::scoped_lock<std::mutex> lock(state->mutex);
            state->result = std::move(r);
        }
        std::function<void(const Result<ValueT> &)> to_call;
        auto maybe = detail::take_for_delivery(*state, to_call);
        if (maybe) {
            auto out = std::move(*maybe);
            detail::post_to_main([to_call, out]() mutable -> void { to_call(out); });
        }
    });

    return Task<ValueT>{std::move(state)};
}

}  // namespace aurora
