#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/state/state.h"

namespace aurora {

/// @brief 动作：类型化字符串标签 + 类型擦除载荷。
///
/// Reducer 通过 `type` 区分动作，用 `payload_as<T>()` 安全取回载荷。
/// 对应 specification/02-state.md §4 单向数据流（Redux 式 dispatch/reducer）。
///
/// @code
/// store->dispatch(Action{"setCount", 42});
/// store->dispatch(Action{"increment"});
/// @endcode
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Action {
    std::string type;  ///< 动作类型标签，Reducer 据此区分动作
    std::shared_ptr<void> payload;  ///< 类型擦除载荷；无载荷动作为空指针

    /// @brief 带载荷的动作（载荷按值移动进类型擦除容器）。
    /// @tparam T 载荷值类型，由实参推导。
    /// @param t 动作类型标签。
    /// @param value 载荷值；移动进 `payload` 并类型擦除存储。
    template <typename T>
    Action(std::string t, T value) : type(std::move(t)), payload(std::make_shared<T>(std::move(value))) {}

    /// @brief 无载荷的动作（如 "increment"）。
    /// @param t 动作类型标签。
    explicit Action(std::string t) : type(std::move(t)) {}

    /// @brief 载荷为空时返回 nullptr；类型由调用方保证匹配（本函数不做类型校验）。
    /// @tparam T 期望取回的载荷类型；与存入类型不符时结果为未定义行为，由调用方保证。
    /// @return 载荷的只读指针；`payload` 为空时返回 nullptr。
    template <typename T>
    [[nodiscard]] auto payload_as() const -> const T * {
        if (!payload) {
            return nullptr;
        }
        return static_cast<const T *>(payload.get());
    }
};

/// @brief Reducer：纯函数 `(state, action) -> newState`，必须无副作用，同输入须产生同输出。
/// @tparam S 被归并的状态类型（与 Store\<S\> 的状态类型一致）。
template <typename S>
using Reducer = std::function<S(const S &, const Action &)>;

/// @brief 单向数据流 store（Redux 式）。
///
/// - `dispatch(Action)`：经 reducer 计算新状态，更新内部值并通知订阅者；
/// 同时把新值写入内部 `State<S>`，使订阅本 store 的 `Effect` 触发定点刷新
/// （与现有细粒度信号系统无缝衔接，widget 可像订阅 `State` 一样订阅 store）。
/// - `subscribe(Listener)`：注册状态变化监听（返回取消句柄）。
/// - `as_signal()`：暴露为 `State<S>` 信号视图，供 widget 属性直接绑定。
///
/// @tparam S 状态类型（须可拷贝/移动，且无悬空引用）。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename S>
class Store {
  public:
    using StateType = S;  ///< 对外暴露的状态类型别名（与模板参数 S 一致）
    using Listener = std::function<void(const S &, const S &)>;  ///< (newState, prevState)

    /// @brief 由初始状态与 reducer 构造；同时建立内部响应式镜像 `State<S>`。
    /// @param initial 初始状态（移入）。
    /// @param reducer 每次 dispatch 时计算新状态的纯函数。
    Store(S initial, Reducer<S> reducer)
        : state_(std::move(initial)), reducer_(std::move(reducer)), signal_(std::make_shared<State<S>>(state_)) {}

    /// @brief 读取当前状态（const 引用，零拷贝）。
    /// @return 当前状态的只读引用。
    [[nodiscard]] auto get_state() const -> const S & { return state_; }

    /// @brief 派发动作：计算新状态 → 通知订阅者 + 触发响应式刷新。
    /// @param action 待派发的动作；reducer 依 `action.type` 决定状态迁移。
    auto dispatch(const Action &action) -> void {
        S next = reducer_(state_, action);
        S prev = std::move(state_);
        state_ = std::move(next);
        signal_->set(state_);  // 触发依赖本 store 的 Effect 定点刷新
        for (Listener &l : listeners_) {
            if (l) {
                l(state_, prev);
            }
        }
    }

    /// @brief 订阅状态变化；返回取消订阅的句柄（调用即移除监听）。
    /// @param l 监听器，每次 dispatch 后以 (新状态, 旧状态) 调用。
    /// @return 取消句柄；调用一次即惰性移除本监听。
    [[nodiscard]] auto subscribe(Listener l) -> std::function<void()> {
        listeners_.push_back(std::move(l));
        const std::size_t idx = listeners_.size() - 1;
        return [this, idx]() -> auto {
            if (idx < listeners_.size()) {
                listeners_[idx] = nullptr;  // 惰性移除，避免迭代期重分配
            }
        };
    }

    /// @brief 暴露为响应式信号视图（供 widget 直接订阅，状态变化触发定点刷新）。
    /// @return 内部 `State<S>` 镜像的共享指针；每次 dispatch 后其值与 store 状态同步。
    [[nodiscard]] auto as_signal() -> std::shared_ptr<State<S>> { return signal_; }

  private:
    S state_;
    Reducer<S> reducer_;
    std::shared_ptr<State<S>> signal_;
    std::vector<Listener> listeners_;
};

/// @brief 便捷工厂：生成共享所有权的 Store。
/// @tparam S 状态类型。
/// @param initial 初始状态（移入）。
/// @param reducer 纯函数 reducer（每次 dispatch 计算新状态）。
/// @return 新建 `Store<S>` 的共享指针。
template <typename S>
[[nodiscard]] auto make_store(S initial, Reducer<S> reducer) -> std::shared_ptr<Store<S>> {
    return std::make_shared<Store<S>>(std::move(initial), std::move(reducer));
}

}  // namespace aurora
