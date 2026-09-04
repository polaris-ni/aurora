#pragma once

#include <functional>

#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 双向绑定引用：指向某个上游 State，用于表单控件把值写回上游。
 *
 * 非拥有（// non-owning），仅持有上游 State 的指针。对应 specification/02-state.md §2.1 `Binding<T>`。
 *
 * 可选的「删除回调」（`remover_`）由后端（如 `Preferences::binding`）注入，使绑定
 * 到持久化键的控件可在卸载/失效时通过 `remove()` 删除对应键（多进程下走可靠墓碑语义）。
 * 未注入删除回调的纯 `State` 绑定时，`remove()` 为空操作。
 *
 * @tparam T 绑定值的类型。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename T>
class Binding {
  public:
    Binding() = default;

    explicit Binding(State<T> &upstream) : target_(&upstream) {}

    /// @brief 绑定到上游 State，并可注入一个删除回调（用于删除持久化键）。
    Binding(State<T> &upstream, std::function<void()> remover) : target_(&upstream), remover_(std::move(remover)) {}

    [[nodiscard]] auto get() const -> const T & {
        AURORA_ASSERT(target_ != nullptr, "Binding accessed before bound to a State");
        return target_->get();
    }

    auto set(T v) -> void {
        AURORA_ASSERT(target_ != nullptr, "Binding accessed before bound to a State");
        target_->set(std::move(v));
    }

    /// @brief 删除绑定键（若有注入的删除回调）。无回调时为安全空操作。
    /// 调用后本 Binding 即失效（上游 State 可能被销毁），不应再 `get`/`set`。
    auto remove() const -> void {
        if (remover_) {
            remover_();
        }
    }

    /// @brief 是否注入了删除回调（即该绑定可删除其对应键）。
    [[nodiscard]] auto removable() const -> bool { return static_cast<bool>(remover_); }

    /// @brief 暴露底层 State 信号指针（供控件 collectSignals 订阅外部状态变化）。
    /// 未绑定时返回 nullptr。
    [[nodiscard]] auto target() const -> State<T> * { return target_; }

    [[nodiscard]] auto bound() const -> bool { return target_ != nullptr; }

  private:
    State<T> *target_ = nullptr;  // non-owning
    std::function<void()> remover_;  // 可选：删除对应持久化键（默认空=空操作）
};

}  // namespace aurora
