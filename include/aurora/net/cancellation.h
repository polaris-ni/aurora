#pragma once

/// @brief 异步网络操作的取消令牌（CHG-010 底座切片）。
/// @file net/cancellation.h
///
/// 一次网络操作的「能不能中途放弃」由 `Cancellation` 表达：它是**共享状态**的句柄，拷贝只
/// 增加引用，不复制状态——因此发起方、挂起的协程与引擎内部看到的都是同一个取消位。三条契约：
///
/// - **取消不抛异常**：本库无异常路径，取消一律以 `Result<T>` 的 `net-cancelled` 形态抵达调用方。
/// - **协程必须被恢复**：取消后挂起点**一定**继续执行并求得取消结果。这与 `Task::cancel()`
///   的「回调不再触发」刻意不同——协程帧只能靠 `resume` 前进到 `co_return` 正常析构，
///   「不恢复」等于泄漏帧。
/// - **先释放资源、再投递结果**：引擎登记的钩子负责放行挂起点，钩子在**调用 `cancel()` 的
///   线程**上执行，故钩子实现必须先释放句柄再投递（回调里就地重连时不能撞上刚被占满的句柄表）。
///
/// @code
/// aurora::net::Cancellation token;
/// token.on_cancel([strong = weak.lock()]() mutable { /* 引擎：以 net-cancelled 放行挂起点 */ });
/// token.cancel();                      // 幂等；重复调用不会二次触发钩子
/// @endcode
/// @note Thread: thread-safe（取消位原子、钩子登记互斥保护）
/// @note Side-effects: `cancel()` 触发已登记的钩子（至多一次）

#include <functional>
#include <memory>

namespace aurora::net {

namespace detail {
class CancellationState;  ///< 取消共享状态（定义在实现侧，公共头不暴露其字段与依赖）
}

/// @brief 异步操作的取消令牌（共享状态的句柄）。
/// @note Thread: thread-safe
/// @note Side-effects: none（构造与拷贝不触发任何回调）
class Cancellation {
  public:
    /// @brief 构造一个独立的未取消令牌。
    Cancellation();

    /// @brief 拷贝构造：与新句柄共享同一取消状态（同一操作的多观察者视角）。
    /// @param other 被共享的源令牌。
    Cancellation(const Cancellation &other) = default;

    /// @brief 拷贝赋值：接管源令牌的状态（原状态若无其它持有者即释放）。
    /// @param other 被共享的源令牌。
    /// @return 本对象引用。
    auto operator=(const Cancellation &other) -> Cancellation & = default;

    /// @brief 移动构造：状态归属移交（共享状态句柄，语义等同拷贝）。
    /// @param other 被移空的源令牌。
    Cancellation(Cancellation &&other) noexcept = default;

    /// @brief 移动赋值：同拷贝语义，源句柄仍可安全使用。
    /// @param other 被移空的源令牌。
    /// @return 本对象引用。
    auto operator=(Cancellation &&other) noexcept -> Cancellation & = default;

    /// @brief 析构：只减少共享状态引用，不触发取消也不等待钩子。
    ~Cancellation();

    /// @brief 是否已被取消。
    /// @return 任一持有者调用过 `cancel()` 时为 true。
    [[nodiscard]] auto is_cancelled() const -> bool;

    /// @brief 请求取消：置位并至多触发一次已登记的钩子。
    ///
    /// 幂等：重复调用不产生额外效果。钩子在**本调用的线程**上同步执行，钩子内不应阻塞。
    auto cancel() -> void;

    /// @brief 登记取消钩子（引擎放行挂起点的入口）。
    ///
    /// 竞态处理：登记时若已取消，则在**当前线程立即**调用 `hook`，不留「登记即失效」的窗口。
    /// 只保留最后登记的一枚钩子（传空 `std::function` 表示撤销登记）。
    /// @param hook 取消时要执行的可调用体；空值表示撤销已登记的钩子。
    auto on_cancel(std::function<void()> hook) -> void;

    /// @brief 是否与另一句柄共享同一取消状态。
    /// @param other 被比较的令牌。
    /// @return 指向同一状态时为 true（用于断言与「同一操作」判等）。
    [[nodiscard]] auto shares_state_with(const Cancellation &other) const -> bool;

  private:
    std::shared_ptr<detail::CancellationState> state_;  ///< 共享状态（永不为空）
};

}  // namespace aurora::net
