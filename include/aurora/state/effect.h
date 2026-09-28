#pragma once

#include <functional>
#include <vector>

#include "aurora/state/signal_view.h"
#include "aurora/state/state_registry.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

class SignalViewBase;

/// @brief 依赖边快照（供 StateGraph 输出，同时保留锚点以探测失效）。
struct EffectDep {
    SignalViewBase *raw;  ///< 被读取的信号（仅用于显示地址）
    std::weak_ptr<ReactiveAnchor> anchor;  ///< 其生命周期锚点（失效则跳过）
};

/// @brief 副作用单元：在 run() 期间读取的信号会自动登记为依赖。
///
/// 对应 specification/02-state.md §2.4 信号依赖追踪。widget 在 mount 时为每个响应式属性创建一个 Effect，
/// 属性变化时 Effect 重跑 → markNeedsLayout/Paint（定点刷新，无 key/diff）。
///
/// 生命周期：每个 Effect 持有一个共享锚点 `anchor_`，State 端以
/// `Connection`（弱引用锚点）记录观察边；Effect 析构时锚点释放，State 下一次
/// notify() 探测到失效并惰性摘除，从而无论 State 与 Effect 谁先析构都不会再
/// 解引用失效对象（彻底消除此前双向裸指针悬垂隐患）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class Effect {
  public:
    /// @brief 当前正在执行的 Effect（线程局部，保证多线程安全）。
    /// @return 活跃 Effect 的指针；不在 run() 作用域内时为 nullptr。
    [[nodiscard]] static auto current() -> Effect * { return current_; }

    /// @brief 构造副作用单元并立即登记进 StateGraph 注册表（使依赖图能枚举本 Effect 节点）。
    /// @param fn 每次 run() 执行的可调用体；执行期间读取的信号被登记为依赖。
    explicit Effect(std::function<void()> fn) : fn_(std::move(fn)) {
        detail::register_effect(*this, anchor_);  // 构造即登记（与 StateBase 一致）；注册表 append-only（v1
                                                  // 不注销），析构后条目由弱引用锚点标记失效
    }

    /// @brief 执行 fn；执行前清空旧依赖，执行中读取的信号重新登记依赖。
    auto run() -> void {
        if (disposed_) {
            return;
        }
        deps_.clear();
        Effect *prev = current_;
        current_ = this;
        // RAII 守卫：fn 抛出时也必须恢复 current_，否则全局指针悬垂于本 Effect——
        // 析构后任何 State/Computed::get() 都会向死对象订阅、set() 时解引用已析构对象（UB）。
        // 豁免 cppcoreguidelines-special-member-functions：按 RAII 守卫惯例，作用域守卫不应拷贝/移动
        // （拷贝两个持有同一 prev 的副本会互相覆写 current_），生命周期由析构独占、恢复 prev 即其唯一
        // 职责；补四件套反而诱导误用，此处刻意只写析构。
        // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
        struct CurrentGuard {
            Effect *prev;
            ~CurrentGuard() { current_ = prev; }
        } guard{prev};
        if (fn_) {
            fn_();
        }
    }

    /// @brief 由 SignalView::get() 调用，登记依赖（含锚点以支持失效探测）。
    /// @param s 本次 run() 中被读取的信号视图；连同其锚点追加进 deps_。
    auto add_dep(SignalViewBase &s) -> void { deps_.push_back(EffectDep{.raw = &s, .anchor = s.anchor()}); }

    /// @brief 是否已释放（供 State::notify 跳过失效观察者）。
    /// @return 已 dispose 时为 true（此后 run() 短路为 no-op）。
    [[nodiscard]] auto is_disposed() const noexcept -> bool { return disposed_; }

    /// @brief 释放本 Effect：置 disposed_ 并清空依赖边（重复调用安全）。
    /// 释放后 run() 短路为 no-op，State 端通知路径据此惰性摘除观察边。
    auto dispose() -> void {
        disposed_ = true;
        deps_.clear();  // 清空依赖边快照；State 端观察边由其锚点失效后惰性摘除
    }

    /// @brief 析构时自动 dispose()，摘除与 State 的连接责任由锚点失效机制兜底。
    ~Effect() { dispose(); }

    /// @brief 禁拷贝：锚点与观察者边绑定实例身份，拷贝会产生双重登记。
    Effect(const Effect &) = delete;
    /// @brief 禁拷贝赋值：理由同拷贝构造（防双重登记）。
    auto operator=(const Effect &) -> Effect & = delete;
    /// @brief 禁移动：State 端 Connection 以裸指针+锚点绑定本实例身份，移动会破坏该绑定。
    Effect(Effect &&) = delete;
    /// @brief 禁移动赋值：理由同移动构造（防破坏裸指针+锚点绑定）。
    auto operator=(Effect &&) -> Effect & = delete;

    /// @brief 生命周期锚点（供 Connection 以 weak_ptr 引用）。
    /// @return 构造时建立的共享锚点句柄，永不为空。
    [[nodiscard]] auto anchor() const -> AnchorPtr { return anchor_; }

  private:
    inline static thread_local Effect *current_ = nullptr;

    std::function<void()> fn_;  ///< 每次 run() 执行的可调用体（可为空）
    std::vector<EffectDep> deps_;  ///< 最近一次 run() 登记的依赖边快照（run 前清空重建）
    bool disposed_ = false;  ///< dispose() 后为 true：run() 短路、State 端据此摘边
    AnchorPtr anchor_{make_anchor()};  ///< 本实例的生命周期锚点（State 端弱引用探测用）

    friend class StateGraph;
};

}  // namespace aurora
