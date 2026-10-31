#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/main_queue.h"
#include "aurora/core/result.h"
#include "aurora/core/thread_pool.h"

namespace aurora {

namespace detail {

/// @brief `async` 内部共享状态（在 `Task` 与后台 job 间共享，生命周期由 `shared_ptr` 管理）。
/// @tparam T 任务值类型（由 `TaskValueOfT` 从 fn 返回类型萃取）。
template <typename T>
struct AsyncState {
    std::mutex mutex;  ///< 保护 result/on_done/on_cancel/delivered/epoch 的读写
    std::condition_variable ready;  ///< result 就绪或被取消时唤醒 `blocking_wait` 等待方
    /// @brief 完成回调：结果就绪且可投递时被调用
    std::function<void(const Result<T> &)> on_done;
    /// @brief 取消观察者：`cancel()` 置位后恰触发一次（形态与 `net::Cancellation::on_cancel` 同构）。
    ///
    /// 取消这件事对本层历来只有「结果不再投递」这一半效果；本字段把它补成可订阅的事实，
    /// 于是承接方（如网络交换）能在同一刻放手，而不必轮询 `cancelled` 或等自己的超时。
    std::function<void()> on_cancel;
    std::optional<Result<T>> result;  ///< 后台计算完成后的结果（未就绪则为空）
    std::atomic<bool> cancelled{false};  ///< 取消标志：置位后不再向回调投递
    bool delivered{false};  ///< 已向回调投递（成功/取消/超时）避免重复
    std::uint64_t initiation_epoch{0};  ///< 发起时捕获的值班世代；投递须携带它（落地验世）
};

/// @brief 经进程级唯一队列回投主线程（`aurora::MainQueue` 的形态别名，保留旧调用名）。
///
/// 无值班/世代失配 = 打烊取消：任务**不执行**、记计数诊断、不 terminate——绝无「就地执行」
/// 回退（旧语义会在 worker 线程跑回调 = 静默跨线程写 State，本头改造正为根除它）。
/// @param fn 待回投主线程的可调用对象；由值班循环在主线程执行。
inline auto post_to_main(std::function<void()> fn) -> void { MainQueue::post(std::move(fn)); }

/// @brief 发起侧的三分闸门（`async()` 与 `Task::then()` 共用；specification/01-core.md §8.2）。
///
///  1. 在岗 ⇒ 返回当前世代号，发起时捕获、投递闭包随身携带（落地晚于打烊 = 取消）；
///  2. 打烊窗口（`loop_end` 收尾排水中）⇒ 返回 0 = **出生即取消**——收尾续体链式再发起是
///     合法竞态（长轮询/重试链接力），正常关窗不得打成崩溃；调用方须立即构造已取消状态；
///  3. 其余无值班 ⇒ 纯编程错误 ⇒ `on_contract_violation` 硬失败在元凶栈上（FATAL 日志，
///     默认 terminate，测试经 `set_strict_failure_handler` 注入捕获）。
/// @return 在岗世代号；0 = 出生即取消（调用方据此短路）。
inline auto initiate_or_epoch() -> std::uint64_t {
    const auto epoch = MainQueue::current_epoch();
    if (epoch != 0) {
        return epoch;  // 在岗：正常发起，携带本世世代
    }
    if (MainQueue::is_closing()) {
        return 0;  // 打烊窗口：出生即取消，交由调用方短路成已取消状态
    }
    on_contract_violation("async: task initiated with no main loop on duty (run inside an Application frame loop)");
}

/// @brief 超时看守登记项：到期时刻 + 触发动作（类型擦除，动作自持对应 `AsyncState`）。
struct TimeoutGuard {
    std::chrono::steady_clock::time_point deadline;  ///< 到期时刻（steady_clock），<= now 即应触发
    std::function<void()> expire;  ///< 到点触发的动作（类型擦除，自持对应 AsyncState）
};

/// @brief 看守登记表互斥锁（进程级单例存储）。
/// @return 进程级静态 mutex 的引用。
inline auto timeout_guards_mutex() -> auto & {
    static std::mutex m;
    return m;
}
/// @brief 到期看守登记表：**仅** deferred 线程池构建使用——有 worker 时看守是
/// 一个睡在后台线程上的池任务，无需主线程扫描。见 `Task<T>::with_timeout`。
/// @return 进程级看守向量单例的引用（调用方须自行持 `timeout_guards_mutex()`）。
inline auto timeout_guards() -> std::vector<TimeoutGuard> & {
    static std::vector<TimeoutGuard> v;
    return v;
}

/// @brief 登记一个到期看守（`with_timeout` 的 deferred 分支）。条目活到自己到期为止：
/// 任务提前完成时它照常到期，只是触发时对 `delivered` 短路成 no-op，故表长上界 = 窗口 `d`。
/// @param deadline 到期时刻（登记时按 `now + d` 计算）。
/// @param expire 到点触发的动作；须自持共享状态，可安全重复触发。
inline auto register_timeout_guard(std::chrono::steady_clock::time_point deadline, std::function<void()> expire)
    -> void {
    std::scoped_lock lock(timeout_guards_mutex());  // 加锁写看守表
    timeout_guards().push_back(TimeoutGuard{.deadline = deadline, .expire = std::move(expire)});  // 追加登记项
}

/// @brief 最近登记的到期时刻距 `now` 的毫秒数；表空返回 `-1`（= 无看守，不参与唤醒决策）。
/// @param now 本帧参考时刻。
/// @return 距最近到期的毫秒数（已到期钳 0）；表空为 -1。
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

/// @brief 帧尾扫描：摘出全部到期看守并在锁外逐个触发。
///
/// 必须由宿主在**主线程安全点**每帧调用（`Application::step_frame()` 帧尾），且与
/// `ThreadPool::pump()` 同处一帧——deferred 构建下这是超时唯一能生效的地方：主线程不睡
/// 在看守任务里，也不会被看守任务阻塞。锁外触发是因为 `expire` 会回投主线程并可能登记新项。
/// @param now 本帧参考时刻；`deadline <= now` 的看守被摘出触发。
/// @return 本轮触发的看守数。
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

/// @brief 让超时生效：结果仍未投递且未取消时，写入 `async-timeout` 错误并经进程级唯一队列回投回调。
/// @tparam T 任务值类型。
/// @param state 与后台 job 共享的异步状态（共享指针，看守自持以活到触发为止）。
template <typename T>
auto expire_timeout(const std::shared_ptr<AsyncState<T>> &state) -> void {
    std::function<void(const Result<T> &)> to_call;
    std::optional<Result<T>> err;
    std::uint64_t epoch = 0;
    {
        std::scoped_lock<std::mutex> lock(state->mutex);
        if (state->delivered || state->cancelled) {
            return;  // 已完成或已取消，超时无效
        }
        state->delivered = true;
        state->result = make_error(ErrorCode::RuntimeAsyncTimeout, "async task timed out");
        err = state->result;
        to_call = state->on_done;
        epoch = state->initiation_epoch;
        state->ready.notify_all();  // 唤醒可能在 `blocking_wait` 的等待方
    }
    if (to_call && err) {
        auto r = std::move(*err);
        MainQueue::post([to_call, r]() mutable -> void { to_call(r); }, epoch);
    }
}

/// @brief 首写即定地落一个终值结果并唤醒 `blocking_wait`：已有终值（超时/取消先到）则不覆盖。
/// @tparam T 任务值类型。
/// @param st 共享状态（调用方**须持 `st.mutex`**）。
/// @param r 待落的终值。
template <typename T>
auto set_result_once_locked(AsyncState<T> &st, Result<T> r) -> void {
    if (!st.result.has_value()) {
        st.result = std::move(r);
    }
    st.ready.notify_all();
}

/// @brief 若可投递（结果就绪 + 已注册回调 + 未取消/未投递），复制出并标记 delivered；
/// 返回待投递结果（不可投递则返回 nullopt）。`cb` 取出已注册回调。
/// @tparam T 任务值类型。
/// @param st 共享异步状态；命中投递窗口时其 `delivered` 被置位。
/// @param cb 出参：命中时写入已注册的完成回调，供锁外调用。
/// @return 待投递结果副本；不可投递（未就绪/已取消/未注册/已投递）时为 nullopt。
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
/// @brief 任务值类型萃取主模板：fn 的裸返回类型即任务值类型。
/// @tparam R fn 的裸返回类型。
template <typename R>
struct TaskValueOf {
    /// @brief 任务值类型。
    /// 成员名 `X::type` 是标准库类型萃取的既定形态（与 std traits 组合时按此名查找），
    /// 非本库命名自由度。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = R;
};
/// @brief 任务值类型萃取特化：fn 返回 `Result<U>` 时任务值类型为 U。
/// @tparam U `Result` 中的值类型。
template <typename U>
struct TaskValueOf<Result<U>> {
    /// @brief 任务值类型。
    /// 同上：`trait::type` 由惯例固定。
    /// NOLINTNEXTLINE(readability-identifier-naming)
    using type = U;
};
/// @brief `TaskValueOf<R>::type` 的便捷别名。
/// @tparam R fn 的裸返回类型。
template <typename R>
using TaskValueOfT = TaskValueOf<R>::type;

/// @brief 安全调用 fn：返回 Result<ValueT>，异常捕获为 make_error(ErrorCode::RuntimeAsyncException, ...)（slug 为
/// `"runtime-async-exception"`）。
/// @tparam F 可调用对象类型。
/// @param f 待执行的后台可调用对象；返回裸 `ValueT` 或 `Result<ValueT>` 皆可。
/// @return 正常时透传/包装的 `Result<ValueT>`；抛异常时为携带 RuntimeAsyncException 的错误结果。
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

/// @brief 轻量异步任务（需求 SPEC.FEAT.CORE.ASYNC-CONCURRENCY.001 / specification/02-state.md §5.1）。
///
/// 在单线程 UI 约束下，把「后台计算」与「主线程结果回写」解耦：
/// - `async(fn)` 立即返回一个 `Task<T>`，在**有界线程池**（`ThreadPool::default_pool()`）
/// 的后台 worker 执行 `fn`；不再为每次调用 `std::thread().detach()` 起 OS 线程。
/// - 结果就绪后，经 `then(onDone)` 把回调调度回「主线程」（通过进程级唯一队列 `MainQueue`），
/// 可回写 `State<T>` 以触发响应式定点刷新（与现有信号系统无缝衔接）。
///
/// 取消语义：`cancel()` 标记任务为已取消，后台线程仍会执行完毕（无法中断任意函数），
/// 但 `then` 回调不会被调用。适用于「不再关心结果」的场景。承接侧要真正放手（掐掉一条在途
/// 连接、归还一个句柄），由 `on_cancel()` 登记的观察者去做——取消因此不只是「丢弃结果」，
/// 也是一条可订阅、且至多触发一次的事实。
///
/// 超时语义：`with_timeout(d)` 注册一个超时看守（worker 池下是一个后台池任务，deferred 池下是
/// 一张到期登记表，由宿主帧尾扫描触发——见 `Task<T>::with_timeout`）；
/// 若 `d` 内任务未 `deliver`，则向 `then` 回调投递 `make_error(ErrorCode::RuntimeAsyncTimeout, ...)`（slug 为
/// `"async-timeout"`）。 与 `cancel` 同限制——无法中断任意 `fn`，仅丢弃/改道结果。
///
/// 回投经进程级唯一队列 `aurora::MainQueue`（specification/01-core.md §8.2）：结果就绪后由在岗
/// 主循环在**主线程**执行 `then` 回调，避免跨线程访问 widget。发起异步须有循环在岗——
/// 无值班的发起（打烊窗口除外）是编程错误，在发起栈上硬失败；晚于打烊落地的结果被「打烊取消」
/// （丢弃并计数，绝不在承接 worker 线程就地执行）。循环外要结果请用 `Task::blocking_wait`。
/// @tparam T 任务值类型（`then` 回调接收 `Result<T>`）。
/// @note Thread: thread-safe with mutex
/// @note Side-effects: none
template <typename T>
class Task {
  public:
    using DoneFn = std::function<void(const Result<T> &)>;  ///< 完成回调签名：接收异步结果 `Result<T>`

    /// @brief 由共享状态构造任务句柄（`async()` 工厂内部调用；后台 job 已在启动时接管状态）。
    /// @param state 与后台 job 共享的 `AsyncState<T>`（shared_ptr 共享所有权）。
    explicit Task(std::shared_ptr<detail::AsyncState<T>> state) : state_(std::move(state)) {}

    /// @brief 注册完成回调（结果经进程级唯一队列回投主线程）。返回自身以便链式。
    ///
    /// 发起闸门（三分，见 `detail::initiate_or_epoch`）：在岗 ⇒ 正常登记并保证回主线程执行；
    /// 打烊窗口 ⇒ 该回调永不触发（状态即刻呈已取消）；其余无值班 ⇒ 编程错误硬失败。
    /// @param cb 完成回调；结果就绪后以该结果调用（若结果已先行 deliver 过则补投到新回调）。
    /// @return 引用 `*this`（链式调用）。
    auto then(DoneFn cb) -> Task & {
        const auto epoch = detail::initiate_or_epoch();
        if (epoch == 0) {
            // 打烊窗口注册：无处安放的回调——按「出生即取消」落定（结果即便已到也不补投：
            // 无在岗执行者，投递只会被打烊取消——与其静默，不如把取消记在状态上）。
            std::scoped_lock<std::mutex> lock(state_->mutex);
            state_->cancelled.store(true, std::memory_order_release);
            state_->delivered = true;
            detail::set_result_once_locked(
                *state_, Result<T>{make_error(ErrorCode::RuntimeCancelled,
                                              "async callback registered while the main loop is closing")});
            return *this;
        }
        // 待投递闭包：空 = 无需补投。用 `std::function<void()>` 而非「回调 + std::optional<结果>」
        // 两件局部量，是为了让结果的拷贝发生在锁内、且不再经 optional→optional 的那次中转——
        // libstdc++ 的 `_Optional_payload`（union 载荷 + 独立 engaged 标志）会让路径敏感的
        // clang-analyzer 在锁内那次赋值处丢失对 payload 的跟踪，随后 `std::move(*replay_r)`
        // 即被判成「隐式构造里给 ok_ 赋了未初始化值」（Linux 口径 unique_findings 里的一条；
        // Windows 侧因 MSVC STL 的实现形态不同而不可见）。补投值本就只在这一处用，闭包是它
        // 最短的载体。
        std::function<void()> replay;
        {
            std::scoped_lock<std::mutex> lock(state_->mutex);
            state_->on_done = std::move(cb);
            if (state_->initiation_epoch == 0) {
                // 直接以状态构造的 Task（不经 async 工厂）在本世补盖戳：投递比对以它为准。
                state_->initiation_epoch = epoch;
            }
            // 若结果此前已 deliver（成功/超时/取消）但当时 on_done 为空，则补投到新回调。
            if (state_->delivered && !state_->cancelled && state_->result.has_value()) {
                DoneFn to_call = state_->on_done;
                Result<T> r = *state_->result;  // 锁内取副本：出锁后 state_ 可被其他线程继续改写
                replay = [to_call, r]() mutable -> void { to_call(r); };
            }
        }
        if (replay) {
            MainQueue::post(std::move(replay), state_->initiation_epoch);
        } else {
            try_deliver();
        }
        return *this;
    }

    /// @brief 取消任务：标记为已取消，`then` 回调不会被调用；已登记的取消观察者在锁外恰触发一次。
    /// 注意：后台线程仍会执行完毕（无法中断任意函数），仅丢弃结果——要真正放手承接侧的资源
    /// （如一条在途连接），由 `on_cancel()` 登记的观察者去做，本函数不等它。
    /// 幂等：第二次调用既不重新置位，也不再触发观察者。
    auto cancel() -> void {
        std::function<void()> hook;
        {
            std::scoped_lock<std::mutex> lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_relaxed)) {
                return;  // 已取消过：置位与观察者都只属于第一次
            }
            state_->cancelled.store(true, std::memory_order_release);
            state_->delivered = true;  // 取消即视为已处理，丢弃后续回调
            hook = std::move(state_->on_cancel);  // 摘出即清空：钩子里没有第二份可被重复触发的东西
            // 必须显式清空源：可调用体小到落进 std::function 内部缓冲（小缓冲优化）时，移动是「克隆」
            // 而非转移，源仍持有一份观察者——留着它，观察者连同其捕获会活到共享状态销毁（自持环）。
            // 与 `CoroTask::promise_type::~promise_type` 同一条纪律。显式置空与实现细节无关。
            state_->on_cancel = nullptr;
        }
        // 锁外触发：钩子按约定要能回头碰这枚状态（读 `is_cancelled()`、或直接投递取消事实），
        // 持锁调用即自死锁。与 `net::Cancellation` 同一条口径——钩子在调用线程上同步执行，不得阻塞。
        if (hook) {
            hook();
        }
    }

    /// @brief 登记取消观察者：`cancel()` 置位那一刻在调用线程上同步触发一次。
    ///
    /// 三条契约（与 `net::Cancellation::on_cancel` 同构）：只保留最后登记的一枚（传空即撤销登记）；
    /// 至多触发一次；**登记时若已取消则在当前线程立即触发**，不留「登记即失效」的窗口。
    /// 观察者不得阻塞，也不得假设自己跑在哪条线程上。
    /// @param hook 取消时要执行的可调用体；空值表示撤销已登记的观察者。
    auto on_cancel(std::function<void()> hook) -> void {
        // 两条路各自把 `hook` 交给唯一的归属：已取消则交给本地这份、立即触发；未取消则登记进状态。
        // 写成「先登记、再按标志回读」会让同一枚可调用体存在两处归属，检查器据此报 use-after-move，
        // 而更糟的是回读那一份在并发登记下未必是自己刚放上去的那枚。
        std::function<void()> fire_now;
        {
            std::scoped_lock<std::mutex> lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_acquire)) {
                fire_now = std::move(hook);  // 竞态纪律：取消先于登记也要有观察者收到这一次事实
            } else {
                state_->on_cancel = std::move(hook);
            }
        }
        if (fire_now) {
            fire_now();  // 锁外触发，与 `cancel()` 同一条口径
        }
    }

    /// @brief 查询是否已取消。
    /// @return 已调用 `cancel()` 时为 true。
    [[nodiscard]] auto is_cancelled() const -> bool { return state_->cancelled.load(std::memory_order_acquire); }

    /// @brief 注册超时：超过 `d` 任务仍未回写，则向 `then` 回调投递 `async-timeout` 错误。
    /// 返回自身以便链式。
    ///
    /// 两条实现路径按线程池模式分流，语义一致（到点改道结果、不中断 `fn`）：
    /// - worker 池：看守是一个睡 `d` 的后台池任务（原有形态），到点自行投递，经进程级唯一队列回投。
    /// - **deferred 池**：无后台线程可睡——睡在池任务里等于睡在主线程泵上，且看守排在被看守
    ///   任务之后（同队 FIFO），永远不可能先跑。故只登记到期时刻，由帧尾 `sweep_due_timeouts`
    ///   扫描触发；等待中的宿主循环经 `next_timeout_deadline_ms` 把期限并入唤醒决策，不深睡过头。
    /// @tparam Rep 时长计数器类型。
    /// @tparam Period 时长比例类型。
    /// @param d 超时窗口；到期时若结果尚未投递则改道为超时错误。
    /// @return 引用 `*this`（链式调用）。
    template <typename Rep, typename Period>
    auto with_timeout(std::chrono::duration<Rep, Period> d) -> Task & {
        auto state = state_;
        auto expire = [state]() -> void { detail::expire_timeout(state); };
        if (ThreadPool::default_pool().is_deferred()) {
            detail::register_timeout_guard(std::chrono::steady_clock::now() + d, std::move(expire));
            return *this;
        }
        ThreadPool::default_pool().execute([expire = std::move(expire), d]() -> void {
            // 可中断等待：进程退出时线程池析构 notify_all 立即唤醒，避免 join 卡满超时窗口。
            ThreadPool::default_pool().interruptible_wait_for(d);
            expire();
        });
        return *this;
    }

    /// @brief 同步等待任务的**终值结果**（在调用线程阻塞），不经投递通道、与值班/世代无关。
    ///
    /// 用途是「循环外也要拿到结果」的合法出口（CLI / 单测 / 结果本就不用于 UI）：
    /// **打烊取消的是投递、不是结果**——worker 落 `result` 与是否在岗无关，故句柄持有者
    /// 在打烊后照样取得本值；「出生即取消」的任务已把取消错误写进结果，本调用立即返回它。
    /// 不会死锁：`then` 回调是经投递排队、只有排水才执行，本调用不等它们（代价是停摆一帧）。
    /// @warning 不要在循环内用它等待 `then` 回调——那是投递语义，要排水才走；本调用只等结果。
    /// @warning 取消（`cancel()`）不中断 `fn`：任务被取消后 `fn` 仍会跑完并落值，本调用会等到那时。
    /// @return 首个终值结果（成功值 / 超时错误 / 取消错误）。
    [[nodiscard]] auto blocking_wait() -> Result<T> {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->ready.wait(lock, [this]() -> bool { return state_->result.has_value(); });
        // 谓词 `has_value()` 成立即返回，故此处必已落值；静态检查看不进 `condition_variable::wait`
        // 的谓词，`*` 与 `value()` 都会被判成未检查访问。
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return *state_->result;
    }

  private:
    /// @brief 若结果已就绪且已注册回调，则投递（去重：仅首次 deliver 调用）。
    /// 携带发起世带：落地若晚于打烊（世代失配）即被打烊取消，绝不在承接 worker 线程就地执行。
    auto try_deliver() -> void {
        std::function<void(const Result<T> &)> to_call;
        auto maybe = detail::take_for_delivery(*state_, to_call);
        if (maybe) {
            auto r = std::move(*maybe);
            MainQueue::post([to_call, r]() mutable -> void { to_call(r); }, state_->initiation_epoch);
        }
    }

    std::shared_ptr<detail::AsyncState<T>> state_;
};

/// @brief 启动异步任务：在后台线程池执行 `fn`（返回 `T` 或 `Result<T>`），返回 `Task<ValueT>`。
/// 返回的 Task 支持 `then()` 回主线程、`cancel()` 取消、`with_timeout()` 超时、`blocking_wait()` 同步取结果。
///
/// 发起闸门（三分，见 `detail::initiate_or_epoch`）：在岗 ⇒ 捕获本世世代、正常排工；
/// 打烊窗口 ⇒ **出生即取消**（不排工，即刻呈已取消，`then` 永不触发、`blocking_wait` 得取消错误）；
/// 其余无值班 ⇒ 编程错误硬失败（循环外发起异步却指望主线程消费，正是要拦的形态）。
/// @tparam F 后台可调用对象类型。
/// @param fn 后台任务体；在 `ThreadPool::default_pool()` 的 worker 上执行。
/// @return `Task<ValueT>` 句柄（ValueT 由 `TaskValueOfT` 从 fn 返回类型萃取）。
template <typename F>
auto async(F &&fn) {
    using ValueT = detail::TaskValueOfT<std::invoke_result_t<F>>;

    auto state = std::make_shared<detail::AsyncState<ValueT>>();
    const std::uint64_t epoch = detail::initiate_or_epoch();
    if (epoch == 0) {
        // 出生即取消：不排工，结果即刻落一枚取消错误并唤醒可能在等的 blocking_wait。
        std::scoped_lock<std::mutex> lock(state->mutex);
        state->cancelled.store(true, std::memory_order_release);
        state->delivered = true;
        detail::set_result_once_locked(
            *state,
            Result<ValueT>{make_error(ErrorCode::RuntimeCancelled, "async task cancelled at birth: loop closing")});
        return Task<ValueT>{std::move(state)};
    }
    state->initiation_epoch = epoch;

    auto state_capture = state;
    auto f = std::forward<F>(fn);

    // NOLINTNEXTLINE(bugprone-exception-escape) 回调由 invoke_safe/execute 内 try/catch 兜底，抛异常不会逃出线程边界
    ThreadPool::default_pool().execute([state = std::move(state_capture), f = std::move(f), epoch]() mutable -> void {
        {
            Result<ValueT> r = detail::invoke_safe(std::move(f));
            std::scoped_lock<std::mutex> lock(state->mutex);
            detail::set_result_once_locked(*state, std::move(r));  // 首写即定 + 唤醒 blocking_wait
        }
        std::function<void(const Result<ValueT> &)> to_call;
        auto maybe = detail::take_for_delivery(*state, to_call);
        if (maybe) {
            auto out = std::move(*maybe);
            // 携带发起世代投递：晚于打烊即被打烊取消，绝不在本 worker 线程就地执行回调。
            MainQueue::post([to_call, out]() mutable -> void { to_call(out); }, epoch);
        }
    });

    return Task<ValueT>{std::move(state)};
}

}  // namespace aurora
