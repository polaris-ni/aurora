#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string_view>

#include "aurora/core/log.h"

namespace aurora {

/**
 * @brief 严格模式（规格 §3.3 / CI 用）。
 *
 * `StrictMode::On` 时，`Diagnostics::degraded` 升级为 **error**（硬失败），
 * 用于 CI 把「降级渲染 / 深度超限」等本应容忍的问题变为构建阻断。
 *
 * 已迁移到 Application 上下文：`au::App().strict_mode(On).run()` 会在运行期
 * 把线程级开关设为 On；`Application` 也提供 `set_strict_mode()` / `strict_mode()`。
 * 旧的全局 `aurora::set_strict_mode()` 仍保留，供无 App 上下文的场景（如单元测试）。
 */
enum class StrictMode : std::uint8_t { Off = 0, On = 1 };

namespace detail {
/// @brief 线程局部存储，避免多线程数据竞争。
inline thread_local auto tl_strict_mode = StrictMode::Off; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// @brief 严格模式失败处理器（可注入，便于测试拦截真实致命失败）。
/// 生产默认（handler 为空）直接 `std::terminate()`；测试可注入抛异常或记录的处理器。
using StrictFailureHandler = std::function<void(std::string_view)>;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline StrictFailureHandler g_strict_failure_handler = nullptr;
} // namespace detail

[[nodiscard]] inline auto strict_mode() -> StrictMode { return detail::tl_strict_mode; }

inline auto set_strict_mode(StrictMode m) -> void { detail::tl_strict_mode = m; }

/// @brief 注入严格模式失败处理器（通常为测试用）。传空恢复默认（std::terminate）。
inline auto set_strict_failure_handler(detail::StrictFailureHandler h) -> void {
    detail::g_strict_failure_handler = std::move(h);
}

/**
 * @brief 严格模式硬失败：打印到 stderr 后触发致命失败。
 *
 * 不依赖 `AURORA_ASSERT`（NDEBUG 下会被剥离），保证 Release/CI 构建也能被阻断。
 * 默认 `std::terminate()`；若注入了 handler，则调用 handler（预期其不返回，
 * 例如测试注入的 handler 抛出 `std::runtime_error` 以便断言捕获）。
 */
[[noreturn]] inline auto on_strict_failure(std::string_view message) -> void {
    AURORA_LOG_FATAL("strict", message);
    if (detail::g_strict_failure_handler) {
        detail::g_strict_failure_handler(message);
        std::terminate(); // handler 未终止则兜底，确保不穿透
    }
    std::terminate();
}

} // namespace aurora
