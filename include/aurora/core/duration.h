#pragma once

#include <chrono>

namespace aurora {

/**
 * @brief 时长强类型（规格 §1.6 互补字面量）。
 *
 * 当前库内时间多为裸 `double` 秒或 `std::chrono`，引入 `Duration` 统一为
 * 编译期单位安全的时长值，避免 magic number 秒与单位歧义。
 */
struct Duration {
    double seconds = 0.0; ///< 以秒存储。

    constexpr Duration() noexcept = default;
    constexpr explicit Duration(double s) noexcept : seconds(s) {}

    [[nodiscard]] static constexpr auto from_seconds(double s) noexcept -> Duration { return Duration{ s }; }
    [[nodiscard]] static constexpr auto from_ms(double m) noexcept -> Duration { return Duration{ m / 1000.0 }; }

    [[nodiscard]] constexpr auto to_chrono() const noexcept -> std::chrono::duration<double> {
        return std::chrono::duration<double>(seconds);
    }

    [[nodiscard]] constexpr auto operator==(const Duration &o) const noexcept -> bool { return seconds == o.seconds; }
    [[nodiscard]] constexpr auto operator!=(const Duration &o) const noexcept -> bool { return !(*this == o); }
};

/**
 * @brief 时长字面量（规格 §1.6）。**仅可在 TU 内显式 `using namespace au::literals;` 后使用**。
 * @code
 *   using namespace au::literals;
 *   auto d = 250_ms;   // 0.25 秒
 * @endcode
 */
namespace literals {
[[nodiscard]] constexpr auto operator""_ms(long double v) noexcept -> Duration {
    return Duration::from_ms(static_cast<double>(v));
}
[[nodiscard]] constexpr auto operator""_ms(unsigned long long v) noexcept -> Duration {
    return Duration::from_ms(static_cast<double>(v));
}
} // namespace literals

} // namespace aurora
