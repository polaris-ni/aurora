#pragma once

#include <functional>

#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 双向绑定引用：指向某个上游 State，用于表单控件把值写回上游。
 *
 * 非拥有（// non-owning），仅持有上游 State 的指针。对应架构 §3.3 `Binding<T>`。
 *
 * 可选的「删除回调」（`m_remover`）由后端（如 `Preferences::binding`）注入，使绑定
 * 到持久化键的控件可在卸载/失效时通过 `remove()` 删除对应键（多进程下走可靠墓碑语义）。
 * 未注入删除回调的纯 `State` 绑定时，`remove()` 为空操作。对应 `SPECIFICATIONS.md` #H.16。
 *
 * @tparam T 绑定值的类型。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename T> class Binding {
  public:
    Binding() = default;

    explicit Binding(State<T> &upstream) : m_target(&upstream) {}

    /// @brief 绑定到上游 State，并可注入一个删除回调（用于删除持久化键）。
    Binding(State<T> &upstream, std::function<void()> remover) : m_target(&upstream), m_remover(std::move(remover)) {}

    [[nodiscard]] auto get() const -> const T & {
        AURORA_ASSERT(m_target != nullptr, "Binding accessed before bound to a State");
        return m_target->get();
    }

    auto set(T v) -> void {
        AURORA_ASSERT(m_target != nullptr, "Binding accessed before bound to a State");
        m_target->set(std::move(v));
    }

    /// @brief 删除绑定键（若有注入的删除回调）。无回调时为安全空操作。
    /// 调用后本 Binding 即失效（上游 State 可能被销毁），不应再 `get`/`set`。
    auto remove() const -> void {
        if (m_remover) {
            m_remover();
        }
    }

    /// @brief 是否注入了删除回调（即该绑定可删除其对应键）。
    [[nodiscard]] auto removable() const -> bool { return static_cast<bool>(m_remover); }

    /// @brief 暴露底层 State 信号指针（供控件 collectSignals 订阅外部状态变化）。
    /// 未绑定时返回 nullptr。
    [[nodiscard]] auto target() const -> State<T> * { return m_target; }

    [[nodiscard]] auto bound() const -> bool { return m_target != nullptr; }

  private:
    State<T> *m_target = nullptr;    // non-owning
    std::function<void()> m_remover; // 可选：删除对应持久化键（默认空=空操作）
};

} // namespace aurora
