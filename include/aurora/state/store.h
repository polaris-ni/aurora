#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 动作：类型化字符串标签 + 类型擦除载荷。
 *
 * Reducer 通过 `type` 区分动作，用 `payloadAs<T>()` 安全取回载荷。
 * 对应 specification/02-state.md §4 单向数据流（Redux 式 dispatch/reducer）。
 *
 * @code
 *   store->dispatch(Action{"setCount", 42});
 *   store->dispatch(Action{"increment"});
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct Action {
    std::string type;
    std::shared_ptr<void> payload;

    /// @brief 带载荷的动作（载荷按值移动进类型擦除容器）。
    template<typename T>
    Action(std::string t, T value) : type(std::move(t)), payload(std::make_shared<T>(std::move(value))) {}

    /// @brief 无载荷的动作（如 "increment"）。
    explicit Action(std::string t) : type(std::move(t)) {}

    /// @brief 取回载荷；类型不匹配或为空时返回 nullptr。
    template<typename T> [[nodiscard]] auto payload_as() const -> const T * {
        if (!payload) {
            return nullptr;
        }
        return static_cast<const T *>(payload.get());
    }
};

/**
 * @brief Reducer：纯函数 `(state, action) -> newState`。
 * 必须无副作用；同输入须产生同输出，便于测试与快照。
 * @tparam S 状态类型（须可拷贝/移动）。
 */
template<typename S> using Reducer = std::function<S(const S &, const Action &)>;

/**
 * @brief 单向数据流 store（Redux 式）。
 *
 * - `dispatch(Action)`：经 reducer 计算新状态，更新内部值并通知订阅者；
 *   同时把新值写入内部 `State<S>`，使订阅本 store 的 `Effect` 触发定点刷新
 *   （与现有细粒度信号系统无缝衔接，widget 可像订阅 `State` 一样订阅 store）。
 * - `subscribe(Listener)`：注册状态变化监听（返回取消句柄）。
 * - `asSignal()`：暴露为 `State<S>` 信号视图，供 widget 属性直接绑定。
 *
 * @tparam S 状态类型（须可拷贝/移动，且无悬空引用）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename S> class Store {
  public:
    using StateType = S;
    using Listener = std::function<void(const S &, const S &)>; ///< (newState, prevState)

    Store(S initial, Reducer<S> reducer)
        : m_state(std::move(initial)), m_reducer(std::move(reducer)), m_signal(std::make_shared<State<S>>(m_state)) {}

    /// @brief 读取当前状态（const 引用，零拷贝）。
    [[nodiscard]] auto get_state() const -> const S & { return m_state; }

    /// @brief 派发动作：计算新状态 → 通知订阅者 + 触发响应式刷新。
    auto dispatch(const Action &action) -> void {
        S next = m_reducer(m_state, action);
        S prev = std::move(m_state);
        m_state = std::move(next);
        m_signal->set(m_state); // 触发依赖本 store 的 Effect 定点刷新
        for (Listener &l : m_listeners) {
            if (l) {
                l(m_state, prev);
            }
        }
    }

    /// @brief 订阅状态变化；返回取消订阅的句柄（调用即移除监听）。
    [[nodiscard]] auto subscribe(Listener l) -> std::function<void()> {
        m_listeners.push_back(std::move(l));
        const std::size_t idx = m_listeners.size() - 1;
        return [this, idx]() -> auto {
            if (idx < m_listeners.size()) {
                m_listeners[idx] = nullptr; // 惰性移除，避免迭代期重分配
            }
        };
    }

    /// @brief 暴露为响应式信号视图（供 widget 直接订阅，状态变化触发定点刷新）。
    [[nodiscard]] auto as_signal() -> std::shared_ptr<State<S>> { return m_signal; }

  private:
    S m_state;
    Reducer<S> m_reducer;
    std::shared_ptr<State<S>> m_signal;
    std::vector<Listener> m_listeners;
};

/// @brief 便捷工厂：生成共享所有权的 Store。
template<typename S> [[nodiscard]] auto make_store(S initial, Reducer<S> reducer) -> std::shared_ptr<Store<S>> {
    return std::make_shared<Store<S>>(std::move(initial), std::move(reducer));
}

} // namespace aurora
