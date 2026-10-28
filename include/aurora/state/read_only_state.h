#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/state/detail/reactor.h"
#include "aurora/state/detail/registry.h"
#include "aurora/state/subscription.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

template <typename T>
class State;  // 前向声明：StateSink 的写句柄登记重载在实例化时解析

template <typename T>
class ReadOnlyState;  // 前向声明：句柄构造通道的返回类型

class StateSink;  // 前向声明：只读句柄的采集入口

namespace detail {

/// @brief 仅库内工厂使用的句柄构造通道：把来源节点包成公共句柄。
///
/// 句柄的私有构造不向用户开放，`computed` 与 `State` 的降级转换经本通道完成。
struct HandleAccess {
    /// @brief 以来源节点构造只读句柄。
    /// @tparam T 值类型。
    /// @param source 来源节点（本句柄与来源共享所有权）。
    /// @return 持有该来源的只读句柄。
    template <typename T>
    static auto read_only(std::shared_ptr<SourceOf<T>> source) -> ReadOnlyState<T>;

    /// @brief 取可写句柄持有的来源节点（供资源投影构造保活原控制核的别名句柄）。
    /// @tparam T 值类型。
    /// @param state 可写句柄。
    /// @return 该句柄共享的来源节点。
    template <typename T>
    static auto source_of(const State<T> &state) -> std::shared_ptr<SourceOf<T>>;
};

}  // namespace detail

/// @brief 只读状态句柄：拥有来源的读视图，写入路径在类型层面不存在。
///
/// - `get()`：读取当前值；在 effect 或派生计算体内读取会自动登记依赖。
/// - `subscribe(fn)`：固定到**调用时的来源**，成功建立后立即交付首值一次，返回可取消凭证。
/// - 由 `T` 隐式构造会创建自有常量来源，支撑展示属性的字面量写法（`content = "Hi"`）。
///
/// 复制句柄共享同一来源；赋值只重绑本句柄，既有订阅仍固定原来源。
/// 与 `State<T>` 单向：`State<T>` 可隐式降级为本类型，反向不成立。
///
/// @tparam T 值的类型。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename T>
class ReadOnlyState {
  public:
    /// @brief 默认构造：创建含 `T{}` 的自有来源；仅当 `T` 可默认构造时可用。
    ReadOnlyState()
        requires std::default_initializable<T>
        : source_(detail::make_source<T>(T{})) {}

    /// @brief 由可转换为 `T` 的值构造：创建自有常量来源。
    ///
    /// 刻意不加 explicit：展示属性依赖字面量隐式转换（如 `const char*` → `LocalizedString`
    /// → `ReadOnlyState<LocalizedString>`），且模板形式避免双重用户定义转换。
    /// @tparam U 可转换为 `T` 的源值类型。
    /// @param initial 源值，转发构造为来源初值。
    template <typename U>
        requires std::convertible_to<U, T> && (!std::same_as<std::remove_cvref_t<U>, ReadOnlyState>)
    ReadOnlyState(U &&initial)  // NOLINT(google-explicit-constructor)
        : source_(detail::make_source<T>(T(std::forward<U>(initial)))) {}

    /// @brief 读取当前值；在活跃依赖收集作用域内读取会自动登记依赖。
    /// @return 当前值的常引用（由来源持有）。
    [[nodiscard]] auto get() const -> const T & { return source_->read(); }

    /// @brief 订阅本句柄固定到的来源，返回可取消凭证。
    ///
    /// 建立后立即以当前值调用一次；此后仅该来源的有效提交触发回调。回调内读取其他
    /// 句柄不会扩充本订阅的来源。
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

  private:
    /// @brief 由来源节点构造（仅供库内工厂与句柄降级使用）。
    /// @param source 来源节点；本句柄与来源共享所有权。
    explicit ReadOnlyState(std::shared_ptr<detail::SourceOf<T>> source) : source_(std::move(source)) {}

    std::shared_ptr<detail::SourceOf<T>> source_;  ///< 来源节点（共享所有权，常量与派生共用）

    friend struct detail::HandleAccess;
    friend class StateSink;
};

namespace detail {

template <typename T>
auto HandleAccess::read_only(std::shared_ptr<SourceOf<T>> source) -> ReadOnlyState<T> {
    return ReadOnlyState<T>(std::move(source));
}

}  // namespace detail

/// @brief 信号采集出口：控件把自己的只读/可写句柄登记给框架，由框架统一订阅。
///
/// 自定义控件在 `collect_signals(StateSink &out)` 中调用 `out.push(member_)` 即可；
/// 采集只登记来源身份，不附加属性名或作用域标签，也不触发任何用户回调。
///
/// 同一来源重复登记会被去重；临时构造的常量句柄由采集结果保活，不产生悬垂来源。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class StateSink {
  public:
    /// @brief 登记只读句柄的来源。
    /// @tparam T 值类型。
    /// @param state 待采集的只读句柄（可为 `State<T>` 经隐式降级后的临时值）。
    template <typename T>
    auto push(const ReadOnlyState<T> &state) -> void {
        add(state.source_);
    }

    /// @brief 登记可写句柄的来源。
    /// @tparam T 值类型。
    /// @param state 待采集的可写句柄。
    template <typename T>
    auto push(const State<T> &state) -> void {
        add(state.source_);
    }

    /// @brief 已登记来源（按首次登记顺序，已去重）。
    /// @return 来源节点的共享句柄列表，本出口与之外共享所有权。
    [[nodiscard]] auto sources() const -> const std::vector<std::shared_ptr<detail::SourceNode>> & { return sources_; }

    /// @brief 为全部已登记来源建立观察：任一来源提交即调用 `on_dirty`。
    ///
    /// 每条订阅在执行体内读取其固定来源（建立且只建立这一条依赖）后调用回调，
    /// 因此来源在订阅建立后被换掉也不会串台。首条订阅建立后立即首跑一次，
    /// 与旧「挂载即跑一遍」的接线语义一致。
    /// @tparam F 无参可调用体。
    /// @param on_dirty 任一来源有效提交后调用的回调（每条订阅各持一份副本）。
    /// @return 与来源一一对应、按登记顺序排列的订阅凭证。
    template <typename F>
        requires std::copy_constructible<std::decay_t<F>> && std::invocable<std::decay_t<F> &>
    [[nodiscard]] auto watch(F &&on_dirty) const -> std::vector<Subscription> {
        std::vector<Subscription> subscriptions;
        subscriptions.reserve(sources_.size());
        // 首个持有者按实参构造，之后每条订阅各拷一份，互不共享可调用体状态。
        const std::decay_t<F> notify_base(std::forward<F>(on_dirty));
        for (const auto &source : sources_) {
            auto record = detail::make_record([source, notify = notify_base]() mutable {
                source->read_erased();  // 固定到采集时的来源：只登记本来源
                notify();
            });
            source->add_observer(*record);
            subscriptions.emplace_back([record, source]() { record->cancel(); });
            record->run_first();
        }
        return subscriptions;
    }

  private:
    /// @brief 追加来源并去重（按稳定节点身份判定）。
    /// @param node 待登记来源的共享句柄。
    auto add(const std::shared_ptr<detail::SourceNode> &node) -> void {
        for (const auto &existing : sources_) {
            if (existing->id() == node->id()) {
                return;
            }
        }
        sources_.push_back(node);
    }

    std::vector<std::shared_ptr<detail::SourceNode>> sources_;  ///< 采集到的来源（去重、共享所有权）
};

}  // namespace aurora
