#pragma once

#include <chrono>

namespace aurora {

/// @brief 时长强类型（需求 SPEC.API.STRONG-TYPES.4 互补字面量）。
///
/// 当前库内时间多为裸 `double` 秒或 `std::chrono`，引入 `Duration` 统一为
/// 编译期单位安全的时长值，避免 magic number 秒与单位歧义。
struct Duration {
    double seconds = 0.0;  ///< 以秒存储。

    constexpr Duration() noexcept = default;

    /// @brief 以秒值直接构造时长。
    /// @param s 秒数（可为任意有限浮点值）。
    constexpr explicit Duration(double s) noexcept : seconds(s) {}

    /// @brief 由秒数构造时长。
    /// @param s 秒数。
    /// @return `seconds == s` 的时长。
    [[nodiscard]] static constexpr auto from_seconds(double s) noexcept -> Duration { return Duration{s}; }

    /// @brief 由毫秒数构造时长。
    /// @param m 毫秒数。
    /// @return 等价的秒值时长（`m / 1000.0`）。
    [[nodiscard]] static constexpr auto from_ms(double m) noexcept -> Duration { return Duration{m / 1000.0}; }

    /// @brief 转换为 std::chrono 秒时长，供定时器/调度接口使用。
    /// @return `std::chrono::duration<double>`（单位：秒）。
    [[nodiscard]] constexpr auto to_chrono() const noexcept -> std::chrono::duration<double> {
        return std::chrono::duration<double>(seconds);
    }

    /// @brief 相等比较：按秒值严格比较。
    /// @param o 右操作数。
    /// @return 二者秒值相等时为 true。
    [[nodiscard]] constexpr auto operator==(const Duration &o) const noexcept -> bool { return seconds == o.seconds; }

    /// @brief 不等比较：对 `operator==` 取反。
    /// @param o 右操作数。
    /// @return 二者秒值不相等时为 true。
    [[nodiscard]] constexpr auto operator!=(const Duration &o) const noexcept -> bool {
        return !(*this == o);
    }  // NOLINT(*-redundant-parentheses)
};

/// @brief 时长字面量（需求 SPEC.API.STRONG-TYPES.4）。**仅可在 TU 内显式 `using namespace au::literals;` 后使用**。
/// @code
/// using namespace au::literals;
/// auto d = 250_ms;   // 0.25 秒
/// @endcode
namespace literals {
[[nodiscard]] constexpr auto operator""_ms(long double v) noexcept -> Duration {
    return Duration::from_ms(static_cast<double>(v));
}
[[nodiscard]] constexpr auto operator""_ms(unsigned long long v) noexcept -> Duration {
    return Duration::from_ms(static_cast<double>(v));
}
}  // namespace literals

}  // namespace aurora
