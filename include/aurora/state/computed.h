#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "aurora/state/effect.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 派生状态：由函数 `f(State...)` 计算，自动追踪其读取的依赖。
 *
 * 内部持有一个 Effect：运行 f 时读取的 State 自动成为依赖；任一依赖变化 →
 * 重算并通知本 Computed 的观察者。对应 specification/02-state.md §2.3。
 *
 * @tparam T 计算结果类型。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename T>
class Computed : public SignalView<T>, public StateBase {
  public:
    explicit Computed(std::function<T()> fn)
        : fn_(std::move(fn)), value_(fn_()), effect_(std::make_shared<Effect>([this]() -> void {
              value_ = fn_();
              notify();
          })) {
        effect_->run();
    }

    [[nodiscard]] auto get() const -> const T & override {
        if (Effect::current() != nullptr) {
            // 同 State：get() const 与 subscribe() 非 const 的接口约束所致。
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            const_cast<Computed *>(this)->subscribe(*Effect::current());
        }
        return value_;
    }

    [[nodiscard]] auto anchor() const -> AnchorPtr override { return StateBase::anchor(); }

    auto subscribe(Effect &e) -> void override {
        // 与 State 一致：去重 + 惰性摘除失效边 + 建立弱引用连接。
        for (auto it = observers_.begin(); it != observers_.end();) {
            if (!(*it)->effect.lock()) {
                it = observers_.erase(it);  // 失效边，摘除
                continue;
            }
            if ((*it)->effect_raw == &e) {
                return;
            }
            ++it;
        }
        const auto conn = std::make_shared<Connection>();
        conn->effect = e.anchor();
        conn->effect_raw = &e;
        conn->state = this->anchor();
        observers_.push_back(conn);
        e.add_dep(*this);
    }

  private:
    std::function<T()> fn_;
    T value_;
    std::shared_ptr<Effect> effect_;
};

/**
 * @brief 派生信号工厂：从可调用体的返回类型推导 `T`，构造 `Computed<T>`。
 *
 * `au::computed([&] { return a.get() + b.get(); })` 等价于
 * `Computed<int>{ std::function<int()>{ ... } }`，省去显式模板参数；两种构造均为合法形式。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
template <typename F>
    requires std::invocable<F>
[[nodiscard]] auto computed(F &&fn) -> Computed<std::remove_cvref_t<std::invoke_result_t<F>>> {
    using T = std::remove_cvref_t<std::invoke_result_t<F>>;
    return Computed<T>(std::function<T()>(std::forward<F>(fn)));
}

}  // namespace aurora
