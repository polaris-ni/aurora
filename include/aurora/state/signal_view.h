#pragma once

#include <memory>

namespace aurora {

class Effect; // 前向声明，避免与 effect.hpp 循环依赖

/// @brief 响应式图的「锚点」：每个 State / Effect 实例持有一个共享锚点，
/// 观察边 Connection 以 weak_ptr 引用它，从而在任一侧析构后都能安全探测对方
/// 是否已失效（消除双向裸指针悬垂，详见 effect.h / state.h）。
struct ReactiveAnchor {};
using AnchorPtr = std::shared_ptr<ReactiveAnchor>;

/// @brief 一条观察边：State(observed) —— Effect(observer)。
/// 双方均以 weak_ptr 引用彼此的锚点，任一侧析构后对应 weak_ptr 失效，
/// State::notify() 在遍历时跳过并惰性摘除失效边，杜绝悬垂解引用。
struct Connection {
    std::weak_ptr<ReactiveAnchor> effect; ///< 观察者 Effect 的锚点（弱引用，用于失效探测）
    Effect *effect_raw = nullptr;         ///< 仅用于 StateGraph 显示；仅当 effect 锁定成功（Effect 存活）时才读取
    std::weak_ptr<ReactiveAnchor> state;  ///< 被观察 State 的锚点（弱引用）
};
using ConnectionPtr = std::shared_ptr<Connection>;

/**
 * @brief 信号视图基类（非模板）：仅提供订阅能力，供 Effect 依赖追踪使用。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class SignalViewBase { // NOLINT(cppcoreguidelines-special-member-functions)
  public:
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
    virtual ~SignalViewBase() = default;

    /// @brief 将给定 Effect 注册为自身变化的观察者（读时由 get() 触发）。
    virtual auto subscribe(Effect &e) -> void = 0;

    /// @brief 在活跃 Effect 作用域下读取值以登记依赖（由 get() 实现）。
    virtual auto read() -> void = 0;

    /// @brief 生命周期安全锚点：返回本信号视图的共享锚点。
    ///        State/Computed 覆写为真实锚点；纯信号视图（如 Reactive，其订阅
    ///        总是委托给内部 State）默认返回 nullptr。
    [[nodiscard]] virtual auto anchor() const -> AnchorPtr { return nullptr; }
};

/**
 * @brief 类型化信号视图：可读取值，并在 Effect 作用域内读取时自动登记依赖。
 * @tparam T 值的类型。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename T> class SignalView : public SignalViewBase {
  public:
    [[nodiscard]] virtual auto get() const -> const T & = 0;

    auto read() -> void override { (void)get(); }
};

} // namespace aurora
