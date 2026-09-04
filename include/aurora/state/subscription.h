#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <utility>

#include "aurora/state/effect.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/store.h"

namespace aurora {

/**
 * @brief RAII 订阅句柄：析构时自动取消订阅，杜绝监听器泄漏。
 *
 * 包装由 `State`/`Reactive`/`Computed`/`Store` 的订阅返回的 `std::function<void()>`
 * 取消句柄。AI 生成代码无需手动保存/调用取消句柄——把返回值留在作用域即可，
 * 离开作用域自动取消，避免重复触发与内存泄漏（#9 调试闭环的基石）。
 *
 * @code
 *   auto sub = au::bind(counter, [](int v){ label->set_text(std::to_string(v)); });
 *   // sub 离开作用域 → 自动取消订阅
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none（仅持有并调用取消句柄）
 * @note Rebuildable: no
 */
class Subscription {
  public:
    Subscription() = default;

    /// @brief 由取消句柄构造；空句柄表示"未订阅/已释放"。
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}

    ~Subscription() { reset(); }

    Subscription(const Subscription &) = delete;
    auto operator=(const Subscription &) -> Subscription & = delete;

    Subscription(Subscription &&o) noexcept : cancel_(std::move(o.cancel_)) { o.cancel_ = nullptr; }
    auto operator=(Subscription &&o) noexcept -> Subscription & {
        if (this != &o) {
            reset();
            cancel_ = std::move(o.cancel_);
            o.cancel_ = nullptr;
        }
        return *this;
    }

    /// @brief 是否持有有效（未取消）的订阅。
    [[nodiscard]] auto active() const -> bool { return static_cast<bool>(cancel_); }

    /// @brief 立即取消订阅（幂等；重复调用安全）。
    auto reset() -> void {
        if (cancel_) {
            auto fn = std::move(cancel_);
            fn();
        }
    }

    /// @brief 放弃所有权并返回底层取消句柄（调用后析构不再取消）。
    [[nodiscard]] auto release() -> std::function<void()> {
        auto fn = std::move(cancel_);
        cancel_ = nullptr;
        return fn;
    }

  private:
    std::function<void()> cancel_;
};

/**
 * @brief 把响应式信号接到回调，返回 RAII `Subscription`。
 *
 * 每当 `src` 变化（set）即调用 `fn(最新值)`；首次调用会立即应用一次当前值。
 * 信号源可为 `State<T>` / `Reactive<T>` / `Computed<T>`（均继承 `SignalView<T>`）。
 * 返回的 `Subscription` 析构时自动取消订阅，且安全摘除底层 Effect（无悬垂指针）。
 *
 * @tparam T 信号值类型。
 * @tparam F 可调用 `(const T&) -> void`。
 *
 * @code
 *   au::State<int> count{0};
 *   auto sub = au::bind(count, [](int v){ label->set_text(std::to_string(v)); });
 *   count.set(1); // → label 文本更新为 "1"
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Rebuildable: no
 */
template <typename T, typename F>
    requires std::invocable<F, const T &>
auto bind(SignalView<T> &src, F &&fn) -> Subscription {
    auto eff = std::make_shared<Effect>([&src, f = std::forward<F>(fn)]() -> auto { f(src.get()); });
    eff->run();  // 首次应用当前值并登记依赖
    // 取消句柄持有 eff 使其存活至 dispose；dispose 会从 src 观察者列表摘除本 Effect。
    return Subscription([eff]() -> auto { eff->dispose(); });
}

/**
 * @brief 把单向数据流 `Store` 接到回调，返回 RAII `Subscription`。
 *
 * 每当 `store.dispatch(action)` 产生新状态即调用 `fn(新状态)`。底层复用
 * `Store::subscribe` 的惰性取消句柄，析构自动取消。
 *
 * @tparam S 状态类型。
 * @tparam F 可调用 `(const S&) -> void`。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: no
 */
template <typename S, typename F>
    requires std::invocable<F, const S &>
auto bind(Store<S> &store, F &&fn) -> Subscription {
    auto cancel = store.subscribe([f = std::forward<F>(fn)](const S &next, const S &) -> auto { f(next); });
    return Subscription(std::move(cancel));
}

}  // namespace aurora
