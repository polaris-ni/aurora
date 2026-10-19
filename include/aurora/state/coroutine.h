#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "aurora/state/async.h"

namespace aurora {

namespace detail {
/// @brief 协程共享完成状态：在 promise 与 launch 返回的句柄间共享生命周期。
/// @tparam T 协程任务值类型。
template <typename T>
struct CoroShared {
    std::atomic<bool> done{false};  ///< 协程是否已结束（由 ~promise_type 置位）
    std::optional<Result<T>> result;  ///< 完成结果；结束前为空
    std::mutex mutex;  ///< 保护 on_done 的登记与摘取（帧收尾与登记可在同刻相遇）
    /// @brief 完成观察者：帧收尾时恰触发一次（形态与 `Task<T>::on_cancel` 同构）。
    ///
    /// 协程帧只经 `is_done()` / `result()` 被观察，因此「等一个协程结束」在过去只能靠轮询——本字段
    /// 把收尾这件事补成可订阅的事实，上层的适配器（如把一次网络交换交付成 `Task`）不必再睡每一轮。
    ///
    /// 形参写成**可变**引用而不是 `const`：观察者拿到的是结果槽本身，与 `~promise_type` 触发时交出的
    /// 那一份同源。可拷贝载荷不受影响（照旧按 `const Result<T> &` 接就行，引用能绑上非常量），
    /// 而单所有权载荷（如 `net::WebSocket` 句柄）只有从这里才**搬得走**——`const` 那一版下
    /// `std::move(o.value())` 会退化成调用不存在的拷贝构造，整条订阅路对这类载荷直接闭死。
    /// 搬走后同槽再读即读到已搬空的值，与 `result()` 那条口径逐字同型（两处都是「一次交付只有一位持有者」）。
    std::function<void(Result<T> &)> on_done;
};
/// @brief void 特化：无结果值，仅记录异常收尾时的错误。
template <>
struct CoroShared<void> {
    std::atomic<bool> done{false};  ///< 协程是否已结束（由 ~promise_type 置位）
    std::optional<Error> error;  ///< 异常收尾时的错误；正常完成为空
};
}  // namespace detail

/// @brief 协程式异步任务返回类型（需求 SPEC.FEAT.CORE.ASYNC-CONCURRENCY.001 / specification/02-state.md §5.2
/// 协程路径）。
///
/// 与回调式 `au::async().then()` 并存：`co_await au::co_async(fn)` 在后台线程池执行 `fn`，
/// 续体（coroutine 后续代码）经主线程投递器恢复到主线程（无 poster 时由 worker 直接 resume，
/// 语义等同回调的直接调用）；`fn` 返回 `T` 或 `Result<T>`，`co_await` 表达式求得 `Result<ValueT>`。
///
/// 典型用法：
/// @code
/// au::CoroTask<void> load() {
/// au::Result<Data> r = co_await au::co_async([] { return fetch(); });
/// if (r) store->dispatch(au::Action{"loaded", r.value()});
/// else   Diagnostics::report(r.error().message, {}, r.error().code);
/// }
/// au::launch(load());
/// @endcode
/// @tparam T 协程任务值类型（`result()` 求得 `Result<T>`）。
/// @note 收尾的可观察口有两条：`is_done()` / `result()` 是轮询问法，`on_done()` 是订阅问法。
/// @note Thread: main-thread only (continuation resumes on main thread)
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class CoroTask {
  public:
    /// @brief 协程 promise：承载帧生命周期与完成状态，类型名由 `std::coroutine_traits` 固定。
    ///
    /// `promise_type` 是 `std::coroutine_traits` 固定查找的类型名（改名即协程语法失效），非本库命名自由度；
    /// 协程钩子三件套亦由协议规定，故与 special-member-functions 一并就地豁免。
    /// NOLINTNEXTLINE(*-special-member-functions,readability-identifier-naming)
    struct promise_type {
        /// @brief 与返回句柄共享的完成状态（done 位 + 结果槽），构造即建立。
        /// @return 初始化经 `make_shared` 新建的 `CoroShared<T>`，promise 与句柄共此一份。
        std::shared_ptr<detail::CoroShared<T>> shared = std::make_shared<detail::CoroShared<T>>();

        /// @brief 协程协议：以共享状态构造返回对象。
        /// @return 与本帧共享 `shared` 的 `CoroTask`。
        auto get_return_object() -> CoroTask { return CoroTask{shared}; }
        /// @brief 协程协议：帧创建后立即开始执行（不挂起）。
        ///
        /// 协程协议钩子由标准固定为「在 promise 对象上调用」，非本库设计自由度；
        /// 同体内 return_value/unhandled_exception 必须访问 shared，故三件套统一保持实例方法。
        /// @return `std::suspend_never`（永不首挂）。
        /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto initial_suspend() -> std::suspend_never { return {}; }
        /// @brief 协程协议：收尾不挂起，帧在 co_return 后即刻销毁。
        /// @return `std::suspend_never`（永不终挂）。
        /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto final_suspend() noexcept -> std::suspend_never { return {}; }
        /// @brief 协程协议：`co_return v` 把成功结果写入共享状态。
        /// @param v 协程任务值（移入 `Result<T>` 后存入 `shared->result`）。
        auto return_value(T v) -> void { shared->result = Result<T>{std::move(v)}; }
        /// @brief 协程协议：未捕获异常收尾，折算为 `RuntimeCoroutineException` 错误结果。
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
        /// @brief promise 析构（帧收尾/销毁时触发）：置位 done 唤醒轮询方，并在锁外触发完成观察者。
        /// @note 观察者摘出即清空：留着它，「会话持有共享状态、共享状态又持有会话」就成了自持环。
        /// @note 豁免 bugprone-exception-escape：锁外触发观察者走 std::function 调用，无 noexcept
        /// 规格——即 .clang-tidy 记录在案的 std::function 假告警面（浏览器口径才走通调用图）。
        /// 同一份代码在 native 口径不报（口径差异见 CODING_STANDARDS.md §5.2）。
        /// NOLINTNEXTLINE(bugprone-exception-escape)
        ~promise_type() {
            std::function<void(Result<T> &)> hook;
            {
                std::scoped_lock<std::mutex> lock(shared->mutex);
                shared->done.store(true, std::memory_order_release);
                hook = std::move(shared->on_done);
                // 必须显式清空源：可调用体小到落进 std::function 内部缓冲（小缓冲优化）时，
                // 移动是「克隆」而非转移，源仍持有一份观察者——那就不是「摘出即清空」，观察者连同
                // 其捕获会活到共享状态销毁，正是上一条注释要断的自持环。显式置空与实现细节无关。
                shared->on_done = nullptr;
            }
            // 锁外触发：钩子按约定要能回头销毁这次交付的会话（含本共享状态的另一个持有者），
            // 持锁调用即自死锁。结果槽此时必有值——`return_value` 与 `unhandled_exception` 是仅有的
            // 两条收尾路径（同一枚契约见下方 `result()`），无值即帧被外力毁掉，那不在本库的形态里。
            if (hook && shared->result.has_value()) {
                hook(*shared->result);
            }
        }
    };

    /// @brief 由共享状态构造句柄（仅经 `get_return_object()`/协程编译器使用）。
    /// @param shared 与 promise 共享的完成状态。
    explicit CoroTask(std::shared_ptr<detail::CoroShared<T>> shared) : shared_(std::move(shared)) {}

    /// @brief 协程是否已完成（含异常）。
    /// @return 帧已收尾（promise 析构）时为 true。
    [[nodiscard]] auto is_done() const -> bool { return shared_->done.load(std::memory_order_acquire); }

    /// @brief 协程返回值（仅非 void；异常时返回错误 Result；未完成前调用结果未定义）。
    ///
    /// 豁免本检查：契约要求先 `is_done()`。`return_value` 与 `unhandled_exception` 是协程收尾的必经
    /// 两条路径、都写入 `result`，而 `done` 只在 `~promise_type` 置位——故 `done` 为真时 `result`
    /// 必有值。要改成运行期分支需新增「未完成」公共错误码（现无契合项，属 API 扩张），且不改本检查
    /// 建议的形态；调用方违约即文档化的未定义行为。同一份代码 native 遍不报（口径差异见
    /// CODING_STANDARDS.md §5.2）。
    ///
    /// 载荷不可拷贝（单所有权句柄一类，如 `net::WebSocket`）时结果槽里那一份是**搬走**而不是复制出来的，
    /// 第二次读得到的是已搬空的有效值——这类载荷本就不允许有两个持有者，重复读它没有可主张的意义。可拷贝
    /// 载荷仍走拷贝，「同一枚结果读两次得到同一个值」的既有形态不变。
    /// @return 完成结果的成功/错误载荷（解包 `shared_->result`）。
    /// NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] auto result() const -> Result<T> {
        if constexpr (std::is_copy_constructible_v<Result<T>>) {
            return *shared_->result;
        } else {
            return std::move(*shared_->result);  // NOLINT(bugprone-unchecked-optional-access) 同上契约
        }
    }

    /// @brief 登记完成观察者：帧收尾那一刻以协程的 `Result<T>` 触发一次。
    ///
    /// 三条契约（与 `Task<T>::on_cancel` 同构）：只保留最后登记的一枚（传空即撤销登记）；至多触发一次；
    /// **登记时帧已收尾则在当前线程立即触发**，不留「登记即失效」的窗口。观察者跑在帧收尾的那条线程上
    /// （经主线程投递器恢复的协程即主线程），不得阻塞。
    ///
    /// 本入口存在的理由：`CoroTask` 的 promise 永不首挂也不终挂，帧只能靠 `resume` 前进，结果历来只有
    /// `is_done()` 一条问法——轮询。需要「结束即交付」的适配层（把一次交换交付成 `Task`）因此不必再
    /// 自带一枚节拍，取消与超时照旧由承接侧自己的令牌管。
    ///
    /// 交出的那一份是结果槽的**可变引用**（机理见 `detail::CoroShared<T>::on_done`）：观察者因此既能按
    /// `const Result<T> &` 接住可拷贝载荷（既有形态逐字不变，引用绑得上非常量），也能把单所有权载荷
    /// `std::move` 走——「交付一条自持读环的会话句柄」要的正是后半条，而 `const` 那一版只给得出前半条。
    /// @param hook 完成时要执行的可调用体；空值表示撤销已登记的观察者。
    auto on_done(std::function<void(Result<T> &)> hook) -> void {
        std::optional<Result<T>> late;
        {
            std::scoped_lock<std::mutex> lock(shared_->mutex);
            if (!shared_->done.load(std::memory_order_acquire)) {
                shared_->on_done = std::move(hook);
                return;  // 未收尾：登记即完成，观察者由帧收尾时摘出触发
            }
            if (shared_->result.has_value()) {
                // 已收尾：锁外触发要先把结果取成自己持有的一份。可拷贝载荷取副本（槽里那份留给后来的
                // `result()` 轮询者，同一枚结果读两次仍是同一个值）；单所有权载荷只能搬走，搬走后同槽
                // 即读到已搬空的值——与 `result()` 那处 `if constexpr` 同一条口径。
                if constexpr (std::is_copy_constructible_v<Result<T>>) {
                    late = shared_->result;
                } else {
                    late = std::move(*shared_->result);
                }
            }
        }
        if (hook && late.has_value()) {
            hook(*late);
        }
    }

  private:
    std::shared_ptr<detail::CoroShared<T>> shared_;
};

/// @brief `void` 特化：无返回值，续体仍回主线程。
///
/// 本形态**不提供** `on_done()`：它能交出的只有「已结束」与一枚可选错误，而有值形态那条契约的载荷
/// （`Result<T>`）在这里没有对应物。库内需要订阅收尾的入口（`Client::send` / `open_stream` /
/// `Authenticator::authenticate`）全是 `Result<T>` 形态，故此处不留一枚没有判据可钉的公共口。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <>
class CoroTask<void> {
  public:
    /// @brief 协程 promise（void 形态）：仅记录错误与完成位，类型名由协程协议固定。
    struct promise_type {  // NOLINT(*-special-member-functions,readability-identifier-naming) 同上：协程固定类型名
        /// @brief 与返回句柄共享的完成状态（done 位 + 错误槽），构造即建立。
        /// @return 初始化经 `make_shared` 新建的 `CoroShared<void>`，promise 与句柄共此一份。
        std::shared_ptr<detail::CoroShared<void>> shared = std::make_shared<detail::CoroShared<void>>();

        /// @brief 协程协议：以共享状态构造返回对象。
        /// @return 与本帧共享 `shared` 的 `CoroTask<void>`。
        [[nodiscard]] auto get_return_object() const -> CoroTask { return CoroTask{shared}; }
        /// @brief 协程协议：帧创建后立即开始执行（不挂起）。
        ///
        /// 协程协议钩子：调用形态由标准固定，与 CoroTask<T> 主模板保持一致，勿改 static。
        /// @return `std::suspend_never`（永不首挂）。
        /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto initial_suspend() -> std::suspend_never { return {}; }
        /// @brief 协程协议：收尾不挂起，帧即刻销毁。
        /// @return `std::suspend_never`（永不终挂）。
        /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto final_suspend() noexcept -> std::suspend_never { return {}; }
        /// @brief 协程协议：void 协程 `co_return;` 的收尾钩子（无值可写）。
        ///
        /// 钩子调用形态由标准固定，与主模板保持一致，勿改 static。
        /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        auto return_void() -> void {}
        /// @brief 协程协议：未捕获异常收尾，折算为 `RuntimeCoroutineException` 错误。
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
        /// @brief promise 析构（帧收尾/销毁时触发）：置位 done，供轮询方读取完成状态。
        ~promise_type() { shared->done.store(true, std::memory_order_release); }
    };

    /// @brief 由共享状态构造句柄（仅经 `get_return_object()`/协程编译器使用）。
    /// @param shared 与 promise 共享的完成状态。
    explicit CoroTask(std::shared_ptr<detail::CoroShared<void>> shared) : shared_(std::move(shared)) {}

    /// @brief 协程是否已完成（含异常）。
    /// @return 帧已收尾（promise 析构）时为 true。
    [[nodiscard]] auto is_done() const -> bool { return shared_->done.load(std::memory_order_acquire); }
    /// @brief 异常收尾时记录的错误。
    /// @return 错误可选值；正常完成或未结束时为空。
    [[nodiscard]] auto error() const -> std::optional<Error> { return shared_->error; }

  private:
    std::shared_ptr<detail::CoroShared<void>> shared_;
};

/// @brief 协程等待体：`co_await co_async(fn)` 在后台线程池执行 `fn`，完成后恢复续体。
/// @tparam F 可调用体，返回 `T` 或 `Result<T>`。
/// @note Thread: thread-safe (await_suspend posts to thread pool)
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename F>
struct CoAwaitable {
    /// @brief 任务值类型：由 `F` 的返回类型萃取（`Result<U>` 解包为 `U`）。
    using ValueT = detail::TaskValueOfT<std::invoke_result_t<F>>;

    /// @brief 以后台任务体构造等待体（按值持有，随协程帧存活）。
    /// @param f 后台可调用对象（移入）。
    explicit CoAwaitable(F f) : f_(std::move(f)) {}

    /// @brief awaiter 协议：本等待体总是先挂起、交线程池执行。
    ///
    /// awaiter 三件套：await_suspend/await_resume 必须访问 this，await_ready 保持实例形态以统一调用方式。
    /// @return 恒为 false（始终挂起）。
    /// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] auto await_ready() const -> bool { return false; }  // 始终挂起，交线程池执行

    /// @brief awaiter 协议：把 `fn` 投入线程池，完成后经主线程投递器恢复续体。
    /// @param handle 待恢复的协程句柄；`fn` 落值后由 poster 回主线程（无 poster 时 worker 直接 resume）。
    auto await_suspend(std::coroutine_handle<> handle) -> void {
        // 把 fn 投入线程池；完成后经主线程投递器恢复续体（无 poster 时由 worker 直接 resume）。
        ThreadPool::default_pool().execute([this, handle]() mutable -> void {
            value_ = detail::invoke_safe(std::move(f_));
            detail::post_to_main([handle]() mutable -> void { handle.resume(); });
        });
    }

    /// @brief awaiter 协议：续体恢复后取回任务结果。
    ///
    /// 豁免本检查：`value_` 由 `await_suspend` 投入线程池的任务写入，且写入先于同一任务内的
    /// `handle.resume()`——续体只可能经那条 resume 前进，故 `await_resume` 取用时必有值。分析器看
    /// 不到这条跨线程 happens-before，只能按「可能为空」报；在此加分支等于把协程契约撕成运行期
    /// 判断，无对应错误码可用。同一份代码 native 遍不报（口径差异见 CODING_STANDARDS.md §5.2）。
    /// @return 任务结果（自 `value_` 移出）。
    /// NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto await_resume() -> Result<ValueT> { return std::move(*value_); }

  private:
    F f_;
    std::optional<Result<ValueT>> value_;
};

/// @brief 创建协程等待体：在后台线程池执行 `fn`，`co_await` 求得 `Result<ValueT>`。
/// @tparam F 后台可调用对象类型。
/// @param f 后台任务体（完美转发进 `CoAwaitable`）。
/// @return 可直接 `co_await` 的等待体。
template <typename F>
auto co_async(F &&f) -> CoAwaitable<std::decay_t<F>> {
    return CoAwaitable<std::decay_t<F>>(std::forward<F>(f));
}

/// @brief 启动顶层协程（fire-and-forget）。返回句柄可查询 `is_done()` / `result()`。
/// 协程已在调用 `coro()` 时开始执行（initial_suspend = never），`launch` 仅持有句柄保活。
/// @tparam T 协程任务值类型。
/// @param task 已创建的协程任务句柄。
/// @return 同一句柄（按值转交所有权/保活引用）。
template <typename T>
auto launch(CoroTask<T> task) -> CoroTask<T> {
    return task;
}
/// @brief void 协程重载：语义同主模板，仅持有句柄保活。
/// @param task 已创建的 `CoroTask<void>` 句柄。
/// @return 同一句柄（按值转交）。
template <>
inline auto launch(CoroTask<void> task) -> CoroTask<void> {
    return task;
}

}  // namespace aurora
