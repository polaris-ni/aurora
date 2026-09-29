#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "aurora/state/effect.h"
#include "aurora/state/signal_view.h"
#include "aurora/state/state.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 派生状态：由函数 `f(State...)` 计算，自动追踪其读取的依赖。
///
/// 内部持有一个 Effect：运行 f 时读取的 State 自动成为依赖；任一依赖变化 →
/// 重算并通知本 Computed 的观察者。对应 specification/02-state.md §2.3。
///
/// @tparam T 计算结果类型。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class Computed : public SignalView<T>, public StateBase {
  public:
    /// @brief 由纯计算体构造：立即求值一次并建立依赖追踪。
    /// @param fn 计算体；其执行期间读取的 State 即本派生值的依赖。
    explicit Computed(std::function<T()> fn)
        : fn_(std::move(fn)), effect_(std::make_shared<Effect>([this]() -> void {
              value_ = fn_();
              notify();
          })) {
        effect_->run();  ///< 构造即求值且仅求值一次：value_ 默认构造后由首次 run 填充（副作用 fn 不会被触发两次）
    }

    /// @brief 读取当前派生值；存在活跃 Effect 时自动把本 Computed 登记为其依赖。
    /// @return 最近一次重算得到的值引用（未变化期间稳定）。
    [[nodiscard]] auto get() const -> const T & override {
        if (Effect::current() != nullptr) {
            // 同 State：get() const 与 subscribe() 非 const 的接口约束所致。
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            const_cast<Computed *>(this)->subscribe(*Effect::current());
        }
        return value_;
    }

    /// @brief 生命周期锚点：复用 StateBase 的共享锚点，供观察边弱引用探测。
    /// @return 本实例的锚点句柄（构造即建立，永不为空）。
    [[nodiscard]] auto anchor() const -> AnchorPtr override { return StateBase::anchor(); }

    /// @brief 把给定 Effect 登记为本派生值的观察者。
    /// @param e 待登记的观察者；重复订阅同一 Effect 安全（去重，不重复挂边）。
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
        const auto conn = make_connection();
        conn->effect = e.anchor();
        conn->effect_raw = &e;
        conn->state = this->anchor();
        observers_.push_back(conn);  // 新观察边入表：以 Connection 弱引用双方锚点，随任一侧析构自动失效
        e.add_dep(*this);  // 反向登记：把本 Computed 记入 Effect 的依赖快照，供 StateGraph 输出 depends 边
    }

  private:
    std::function<T()> fn_;
    T value_;
    std::shared_ptr<Effect> effect_;
};

/// @brief 派生信号工厂：从可调用体的返回类型推导 `T`，构造 `Computed<T>`。
///
/// `au::computed([&] { return a.get() + b.get(); })` 等价于
/// `Computed<int>{ std::function<int()>{ ... } }`，省去显式模板参数；两种构造均为合法形式。
///
/// @tparam F 可调用体类型。
/// @param fn 派生计算体；其读取的 State 自动成为依赖。
/// @return 构造并立即求值一次的 `Computed<T>`（T 由 fn 的返回值类型推导）。
/// @note Thread: main-thread only
/// @note Side-effects: none
template <typename F>
    requires std::invocable<F>
[[nodiscard]] auto computed(F &&fn) -> Computed<std::remove_cvref_t<std::invoke_result_t<F>>> {
    using T = std::remove_cvref_t<std::invoke_result_t<F>>;
    return Computed<T>(std::function<T()>(std::forward<F>(fn)));
}

}  // namespace aurora
