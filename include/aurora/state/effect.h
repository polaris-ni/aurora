#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "aurora/state/detail/reactor.h"
#include "aurora/state/detail/registry.h"
#include "aurora/state/subscription.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 副作用工厂：调用即首跑一次，自动追踪执行期间读取的来源。
///
/// 读取的来源任一有效变化后重跑；返回值是唯一的存活凭证：丢弃即首跑后取消，
/// 保留则持续生效，`reset()` 或析构即取消。没有公共 `Effect` 类、`run()` 或
/// 依赖追踪钩子——需要延后启动时移动调用时机，或持有默认构造的 `Subscription`
/// 稍后接管。
///
/// @code
/// auto watcher = au::effect([count] {
///     AURORA_LOG_INFO("demo", count.get());
/// });
/// count.set(1);  // → 回调重跑
/// @endcode
///
/// @tparam F 可调用体，签名等价于 `void()`。
/// @param fn 副作用体；执行期间读取的来源自动成为依赖。
/// @return 存活凭证；析构或 `reset()` 即取消。
/// @note Thread: main-thread only
/// @note Side-effects: 首次调用及依赖变化时执行 `fn`
/// @note Rebuildable: no
template <typename F>
    requires std::invocable<std::decay_t<F> &> && std::copy_constructible<std::decay_t<F>>
[[nodiscard]] auto effect(F &&fn) -> Subscription {
    auto record = detail::make_record(std::function<void()>(std::forward<F>(fn)));
    Subscription subscription([record]() { record->cancel(); });
    record->run_first();
    return subscription;
}

}  // namespace aurora
