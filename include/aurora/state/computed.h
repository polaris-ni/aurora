#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "aurora/state/detail/reactor.h"
#include "aurora/state/detail/registry.h"
#include "aurora/state/read_only_state.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief `computed` 的结果类型：取计算体返回值的退化类型。
/// @tparam F 计算体类型。
template <typename F>
using ComputedResult = std::remove_cvref_t<std::invoke_result_t<std::remove_cvref_t<F> &>>;

/// @brief 派生来源工厂：由计算体创建按需重算的只读状态。
///
/// 计算体执行期间读取的来源自动成为依赖；任一依赖变化后，下一次读取重算并缓存。
/// 结果与缓存值可观察等价时不推进版本，下游据此停止传播。
///
/// @code
/// au::State<int> count{0};
/// auto doubled = au::computed([count] { return count.get() * 2; });
/// au::Text text{au::TextProps{.content = doubled}};
/// @endcode
///
/// @tparam F 计算体类型；返回值即派生值类型。
/// @param fn 纯计算体；执行期间读取的来源自动成为依赖。
/// @return 持有派生来源的只读句柄；首次读取时求值。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename F>
[[nodiscard]] auto computed(F &&fn) -> ReadOnlyState<ComputedResult<F>>
    requires std::invocable<std::remove_cvref_t<F> &> && (!std::is_void_v<ComputedResult<F>>)
{
    using T = ComputedResult<F>;
    auto node = detail::make_derived<T>(std::function<T()>(std::forward<F>(fn)), detail::default_equivalence<T>());
    return detail::HandleAccess::read_only<T>(std::shared_ptr<detail::SourceOf<T>>(std::move(node)));
}

/// @brief 派生来源工厂：以自定义等价策略判定结果是否可观察等价。
/// @tparam F 计算体类型。
/// @tparam Eq 等价判定类型。
/// @param fn 纯计算体；执行期间读取的来源自动成为依赖。
/// @param eq 等价判定，签名等价于 `bool(const T &, const T &)`；返回 true 表示可观察等价。
/// @return 持有派生来源的只读句柄；首次读取时求值。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename F, typename Eq>
[[nodiscard]] auto computed(F &&fn, Eq &&eq) -> ReadOnlyState<ComputedResult<F>>
    requires std::invocable<std::remove_cvref_t<F> &> && (!std::is_void_v<ComputedResult<F>>) &&
             (!std::same_as<std::remove_cvref_t<Eq>, AlwaysPublish>) &&
             std::predicate<std::remove_cvref_t<Eq> &, const ComputedResult<F> &, const ComputedResult<F> &>
{
    using T = ComputedResult<F>;
    auto node = detail::make_derived<T>(std::function<T()>(std::forward<F>(fn)),
                                        std::function<bool(const T &, const T &)>(std::forward<Eq>(eq)));
    return detail::HandleAccess::read_only<T>(std::shared_ptr<detail::SourceOf<T>>(std::move(node)));
}

/// @brief 派生来源工厂：关闭等值去重，每次重算都视为变化。
/// @tparam F 计算体类型。
/// @param fn 纯计算体；执行期间读取的来源自动成为依赖。
/// @param marker `always_publish` 标记。
/// @return 持有派生来源的只读句柄；首次读取时求值。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
template <typename F>
[[nodiscard]] auto computed(F &&fn, AlwaysPublish marker) -> ReadOnlyState<ComputedResult<F>>
    requires std::invocable<std::remove_cvref_t<F> &> && (!std::is_void_v<ComputedResult<F>>)
{
    using T = ComputedResult<F>;
    (void)marker;
    auto node =
        detail::make_derived<T>(std::function<T()>(std::forward<F>(fn)), std::function<bool(const T &, const T &)>{});
    return detail::HandleAccess::read_only<T>(std::shared_ptr<detail::SourceOf<T>>(std::move(node)));
}

}  // namespace aurora
