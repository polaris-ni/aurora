#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "aurora/state/detail/reactor.h"
#include "aurora/state/detail/registry.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/subscription.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 可写状态句柄：拥有来源的读写视图，写权限由类型表达。
///
/// - `get()`：读取当前值；在 effect 或派生计算体内读取会自动登记依赖。
/// - `set(v)`：提交新值并通知依赖；与当前值可观察等价时不替换、不通知。
/// - `update(fn)`：以当前值计算新值再提交；更新回调内隔离外层依赖追踪。
/// - `notify()`：不替换值，强制来源发出一次变更（原地突变的可见化入口）。
/// - `subscribe(fn)`：固定到调用时的来源，立即交付首值一次，返回可取消凭证。
///
/// 复制与右值交接共享同一来源；赋值只重绑本句柄，不影响既有订阅固定的原来源。
/// 可隐式降级为 `ReadOnlyState<T>`，反向不成立。
///
/// `T` 须可拷贝：`set/update` 把候选值放进可拷贝的写队列闭包；持有移动-only 载荷的
/// 类型只能读取，不能经本句柄提交（终止于编译期约束，不会运行期丢写）。
///
/// @tparam T 值的类型（须可拷贝）。
/// @note Thread: main-thread only
/// @note Side-effects: none（`set/update/notify` 变更来源并通知依赖）
/// @note Rebuildable: no
template <typename T>
class State {
  public:
    /// @brief 默认构造：创建含 `T{}` 的自有来源；仅当 `T` 可默认构造时可用。
    State()
        requires std::default_initializable<T>
        : source_(detail::make_source<T>(T{})) {}

    /// @brief 由初值构造，采用默认等价策略（载荷可相等比较时按 `==` 去重）。
    ///
    /// 刻意不加 explicit：属性字段靠这条隐式转换支持 `Text{ .content = "Hi" }` 与
    /// `Checkbox{ true }` 这类写法；加 explicit 会让全部控件的属性直写与默认值初始化失败。
    /// @param initial 初始值（移入来源）。
    State(T initial)  // NOLINT(google-explicit-constructor)
        : source_(detail::make_source<T>(std::move(initial))) {}

    /// @brief 由可转换为 `T` 的值构造：单步隐式转换支撑 `"OK"` → `State<LocalizedString>`。
    ///
    /// 与 `State(T initial)` 并存不产生歧义：实参已是 `T` 时非模板重载更优。
    /// @tparam U 可转换为 `T` 的源值类型。
    /// @param initial 源值，转发构造为来源初值。
    template <typename U>
    State(U &&initial)
        requires std::convertible_to<U, T> &&  // NOLINT(google-explicit-constructor)
                 (!std::same_as<std::remove_cvref_t<U>, State>) && (!std::same_as<std::remove_cvref_t<U>, T>)
        : source_(detail::make_source<T>(T(std::forward<U>(initial)))) {}

    /// @brief 由初值构造并关闭去重：每次提交都通知。
    /// @param initial 初始值（移入来源）。
    /// @param marker `always_publish` 标记，表示不做等值去重。
    State(T initial, AlwaysPublish marker) : source_(detail::make_source<T>(std::move(initial), marker)) {}

    /// @brief 由初值构造并注入自定义等价策略。
    /// @tparam Eq 可调用体，签名等价于 `bool(const T &, const T &)`，返回 true 表示可观察等价。
    /// @param initial 初始值（移入来源）。
    /// @param eq 等价判定；之后同一来源的全部句柄共享该策略。
    template <typename Eq>
    State(T initial, Eq eq)
        requires(!std::same_as<std::remove_cvref_t<Eq>, AlwaysPublish>) && std::predicate<Eq &, const T &, const T &>
        : source_(detail::make_source<T>(std::move(initial), std::move(eq))) {}

    /// @brief 读取当前值；在活跃依赖收集作用域内读取会自动登记依赖。
    /// @return 当前值的常引用（由来源持有）。
    [[nodiscard]] auto get() const -> const T & { return source_->read(); }

    /// @brief 提交新值：可观察等价时不替换、不通知；否则排队写入并通知依赖。
    /// @param next 候选值（送入可拷贝的写队列闭包，故 `T` 须可拷贝）。
    auto set(T next) const -> void { source_->assign(std::move(next)); }

    /// @brief 以当前值计算新值再提交；计算失败时不改变旧值、不发布通知。
    ///
    /// 更新回调内读取其他状态不登记依赖（外层若为 effect 执行体也不受污染）。
    /// @tparam F 可调用体，签名等价于 `T(const T &)`；须为纯计算，不得重入状态写入。
    /// @param fn 更新函数。
    template <typename F>
        requires std::invocable<F &, const T &> && std::convertible_to<std::invoke_result_t<F &, const T &>, T>
    auto update(F &&fn) const -> void {
        detail::SuspendCollection suspended;
        T candidate = std::invoke(std::forward<F>(fn), source_->value());
        source_->assign(std::move(candidate));
    }

    /// @brief 不替换当前值，强制来源发出一次变更。
    ///
    /// 用于 `T` 内部共享可变对象被原地修改后让该修改对订阅者可见；不是关闭去重的
    /// 第二种写法（创建来源时用 `always_publish`），也不穿透派生结果的等值停止。
    auto notify() const -> void { source_->force_publish(); }

    /// @brief 订阅本句柄固定到的来源，返回可取消凭证。
    ///
    /// 建立后立即以当前值调用一次；此后仅该来源的有效提交（含 `notify()`）触发回调。
    /// 回调内读取其他句柄不会扩充本订阅的来源。
    /// @tparam F 可调用体，形参可接受 `const T &`。
    /// @param fn 回调；首值与后续变化都以来源当前值调用。
    /// @return 订阅凭证；析构或 `reset()` 即取消。
    template <typename F>
        requires std::invocable<std::decay_t<F>, const T &>
    [[nodiscard]] auto subscribe(F &&fn) const -> Subscription {
        auto source = source_;
        auto record = detail::make_record([source, callback = std::forward<F>(fn)]() mutable {
            const T &value = source->read();  // 固定来源：登记且只登记本来源
            detail::SuspendCollection suspended;  // 回调读取其他句柄不扩充本订阅
            std::invoke(callback, value);
        });
        source->add_observer(*record);
        Subscription subscription([record, source]() { record->cancel(); });
        record->run_first();
        return subscription;
    }

    /// @brief 降级为只读句柄：共享同一来源，既有订阅与写入相互可见。
    /// @return 持有同一来源的只读句柄。
    operator ReadOnlyState<T>() const {  // NOLINT(google-explicit-constructor)
        return detail::HandleAccess::read_only<T>(source_);
    }

    /// @brief 值赋值：等价于 `set(v)`，**不**更换来源。
    ///
    /// 与拷贝赋值分工明确：`s = value` 提交值（订阅者照常收到通知），`s = other_state`
    /// 重绑句柄（后续写入落到 other_state 的来源）。控件展示属性不靠本赋值直写——
    /// 可见性等展示项经 `set_show(...)` 之类的换源入口修改。
    /// @tparam U 可赋值给 `T` 的源值类型。
    /// @param value 待提交的值。
    /// @return 引用 `*this`。
    template <typename U>
    auto operator=(U &&value) -> State &
        requires(!std::same_as<std::remove_cvref_t<U>, State>) && std::is_assignable_v<T &, U>
    {
        set(T(std::forward<U>(value)));
        return *this;
    }

  private:
    std::shared_ptr<detail::Source<T>> source_;  ///< 来源节点（共享所有权，复制句柄不新增节点）

    friend class StateSink;
    friend struct detail::HandleAccess;
};

namespace detail {

template <typename T>
auto HandleAccess::source_of(const State<T> &state) -> std::shared_ptr<SourceOf<T>> {
    return state.source_;
}

}  // namespace detail

}  // namespace aurora
