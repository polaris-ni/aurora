#pragma once

#include <concepts>
#include <memory>
#include <utility>

#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 响应式属性包装：widget 的属性字段类型，使 `State` 能流入属性。
 *
 * AI 友好：可由值隐式构造，例如 `Text{ .content = "Hi" }`（`LocalizedString` 可由
 * 字符串隐式转换）。get() 在 Effect 作用域内自动登记依赖，set() 触发定点刷新。
 *
 * @tparam T 属性值的类型。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
template <typename T>
class Reactive : public SignalView<T> {
  public:
    Reactive(T v = T{}) : state_(std::make_shared<State<T>>(std::move(v))) {}

    /// @brief 由可转换为 T 的值构造（如 `const char*` → `LocalizedString`），
    /// 使 `Text{ .content = "Hi" }` 这类写法可直接编译（specification/02-state.md §2.2）。
    ///
    /// @note 刻意**不加 explicit**：属性字段就是靠这条隐式转换支持
    ///       `Reactive<Color> bg = Color::blue();` 与 `content = "Hi";`（先隐式构造成
    ///       `Reactive<T>` 再走隐式拷贝赋值）。标 explicit 会让全库 `Text/Button/Slider/
    ///       TextInput` 的赋值与默认值初始化全部编译失败。
    template <typename U>
        requires std::convertible_to<U, T>
    Reactive(U &&u) : state_(std::make_shared<State<T>>(std::forward<U>(u))) {}

    /// @brief 由共享的 `State<T>` 构造：与外部状态共享同一源，变化即触发依赖刷新
    /// （用于 `Provider`/`ThemeScope` 接受 `State<T>` 实现运行时换肤/换区域）。
    explicit Reactive(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

    [[nodiscard]] auto get() const -> const T & override { return state_->get(); }

    auto set(T v) -> void { state_->set(std::move(v)); }

    auto subscribe(Effect &e) -> void override { state_->subscribe(e); }

    /// @brief 取底层 State（供需要直接持有信号的场景）。
    [[nodiscard]] auto state() -> State<T> & { return *state_; }

  private:
    std::shared_ptr<State<T>> state_;
};

}  // namespace aurora
