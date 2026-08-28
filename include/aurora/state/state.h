#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "aurora/state/effect.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/state_registry.h"

namespace aurora {

class StateGraph; // 前向声明（state_graph.h 提供状态依赖图，§2.6）

/// @brief 可观察状态容器基类：持有观察者列表并负责通知。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class StateBase { // NOLINT(cppcoreguidelines-special-member-functions)
  public:
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
    virtual ~StateBase() = default;

  protected:
    StateBase() { detail::register_state(*this, m_anchor); }

    /// @brief 生命周期锚点：供观察边 Connection 以 weak_ptr 引用，使 State 先于
    ///        Effect 析构时也不会留下悬垂观察者（T1b）。
    [[nodiscard]] virtual auto anchor() const -> AnchorPtr { return m_anchor; }

    auto notify() -> void {
        // 遍历并惰性摘除失效/已释放的观察者：Connection 以 weak_ptr 引用 Effect
        // 锚点，任一侧析构后 lock() 失败即为失效，绝不解引用悬垂对象。
        // 注：effect.lock() 仅用于「存活探测」；实际运行/状态查询走 effect_raw（Effect*），
        // 因锚点存活 ⇔ Effect 存活，故 effect_raw 在 lock 成功时必然有效。
        for (auto it = m_observers.begin(); it != m_observers.end();) {
            // NOLINTNEXTLINE
            if (!(*it)->effect.lock()) {
                it = m_observers.erase(it); // 锚点失效（Effect 已析构）→ 摘除
                continue;
            }
            // NOLINTNEXTLINE
            Effect *eff = (*it)->effect_raw;
            if (eff->is_disposed()) {
                it = m_observers.erase(it); // 已释放但未析构 → 摘除
                continue;
            }
            eff->run();
            ++it;
        }
    }

    // protected 是刻意的：派生类（如 State<T>::subscribe）需直接访问观察者表与锚点。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::vector<ConnectionPtr> m_observers;
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    AnchorPtr m_anchor{ std::make_shared<ReactiveAnchor>() };

    friend class Effect;
    friend class StateGraph;
};

/**
 * @brief 细粒度信号状态源（参考 SolidJS signals / Compose mutableStateOf）。
 *
 * - get()：返回值；若在 Effect 作用域内调用，自动把本 State 登记为依赖。
 * - set()：写值并通知所有依赖的 Effect 重跑（定点刷新）。
 *
 * @tparam T 值的类型。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename T>
class State : public SignalView<T>, public StateBase, public std::enable_shared_from_this<State<T>> {
  public:
    explicit State(T v = T{}) : m_value(std::move(v)) {}

    [[nodiscard]] auto get() const -> const T & override {
        if (Effect::current() != nullptr) {
            // 接口约束：get() 为 const 而 subscribe() 为非 const（SignalViewBase），
            // 订阅需修改观察者表，此处去 const 是安全且有意的。
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            const_cast<State *>(this)->subscribe(*Effect::current());
        }
        return m_value;
    }

    auto set(T v) -> void {
        m_value = std::move(v);
        notify();
    }

    [[nodiscard]] auto anchor() const -> AnchorPtr override { return StateBase::anchor(); }

    auto subscribe(Effect &e) -> void override {
        // 去重 + 惰性摘除失效边：Effect::run() 每帧重跑会重新登记依赖，若不过滤
        // 同一 Effect 会在 m_observers 中无限累加（动画场景必现）。同时清理已析构
        // Effect 残留的失效连接。
        for (auto it = m_observers.begin(); it != m_observers.end();) {
            if (!(*it)->effect.lock()) {
                it = m_observers.erase(it); // 失效边，摘除
                continue;
            }
            if ((*it)->effect_raw == &e) {
                return; // 已订阅
            }
            ++it;
        }
        auto conn = std::make_shared<Connection>();
        conn->effect = e.anchor();
        conn->effect_raw = &e;
        conn->state = this->anchor();
        m_observers.push_back(conn);
        e.add_dep(*this);
    }

    /// @brief 返回自身的 shared_ptr 包装（便于传递给子组件做状态提升）。
    /// @note 调用者须确保 State 由 shared_ptr 管理（否则行为未定义）。
    /// @note Thread: main-thread only
    /// @note Side-effects: none
    [[nodiscard]] auto shared() -> std::shared_ptr<State<T>> { return this->shared_from_this(); }

  private:
    T m_value;
};

} // namespace aurora
