#pragma once

#include <functional>
#include <vector>

#include "aurora/state/signal_view.h"

namespace aurora {

class SignalViewBase;

/// @brief 依赖边快照（供 StateGraph 输出，同时保留锚点以探测失效）。
struct EffectDep {
    SignalViewBase *raw;  ///< 被读取的信号（仅用于显示地址）
    std::weak_ptr<ReactiveAnchor> anchor;  ///< 其生命周期锚点（失效则跳过）
};

/**
 * @brief 副作用单元：在 run() 期间读取的信号会自动登记为依赖。
 *
 * 对应 specification/02-state.md §2.4 信号依赖追踪。widget 在 mount 时为每个响应式属性创建一个 Effect，
 * 属性变化时 Effect 重跑 → markNeedsLayout/Paint（定点刷新，无 key/diff）。
 *
 * 生命周期：每个 Effect 持有一个共享锚点 `m_anchor`，State 端以
 * `Connection`（弱引用锚点）记录观察边；Effect 析构时锚点释放，State 下一次
 * notify() 探测到失效并惰性摘除，从而无论 State 与 Effect 谁先析构都不会再
 * 解引用失效对象（彻底消除此前双向裸指针悬垂隐患）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Effect {
  public:
    /// @brief 当前正在执行的 Effect（线程局部，保证多线程安全）。
    [[nodiscard]] static auto current() -> Effect * { return current_; }

    explicit Effect(std::function<void()> fn) : fn_(std::move(fn)) {}

    /// @brief 执行 fn；执行前清空旧依赖，执行中读取的信号重新登记依赖。
    auto run() -> void {
        if (disposed_) {
            return;
        }
        deps_.clear();
        Effect *prev = current_;
        current_ = this;
        if (fn_) {
            fn_();
        }
        current_ = prev;
    }

    /// @brief 由 SignalView::get() 调用，登记依赖（含锚点以支持失效探测）。
    auto add_dep(SignalViewBase &s) -> void { deps_.push_back(EffectDep{.raw = &s, .anchor = s.anchor()}); }

    /// @brief 是否已释放（供 State::notify 跳过失效观察者）。
    [[nodiscard]] auto is_disposed() const noexcept -> bool { return disposed_; }

    auto dispose() -> void {
        disposed_ = true;
        deps_.clear();
    }

    ~Effect() { dispose(); }

    Effect(const Effect &) = delete;
    auto operator=(const Effect &) -> Effect & = delete;
    Effect(Effect &&) = delete;
    auto operator=(Effect &&) -> Effect & = delete;

    /// @brief 生命周期锚点（供 Connection 以 weak_ptr 引用）。
    [[nodiscard]] auto anchor() const -> AnchorPtr { return anchor_; }

  private:
    inline static thread_local Effect *current_ = nullptr;

    std::function<void()> fn_;
    std::vector<EffectDep> deps_;
    bool disposed_ = false;
    AnchorPtr anchor_{std::make_shared<ReactiveAnchor>()};

    friend class StateGraph;
};

}  // namespace aurora
