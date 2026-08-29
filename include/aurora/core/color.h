#pragma once

#include <cstdint>

namespace aurora {

/// @brief 8 位每通道 RGBA 颜色。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Color {
    uint8_t m_r = 0;
    uint8_t m_g = 0;
    uint8_t m_b = 0;
    uint8_t m_a = 255;

    constexpr Color() noexcept = default;

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    constexpr Color(const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a = 255) noexcept
        : m_r(r), m_g(g), m_b(b), m_a(a) {}
    [[nodiscard]] static constexpr auto from_rgba(const uint8_t r, const uint8_t g, const uint8_t b,
                                                  const uint8_t a = 255) noexcept -> Color {
        return Color{ r, g, b, a };
    }
    [[nodiscard]] static constexpr auto white() noexcept -> Color { return Color{ 255, 255, 255 }; }
    [[nodiscard]] static constexpr auto black() noexcept -> Color { return Color{ 0, 0, 0 }; }
    [[nodiscard]] static constexpr auto blue() noexcept -> Color { return Color{ 0, 0, 255 }; }
    [[nodiscard]] static constexpr auto red() noexcept -> Color { return Color{ 255, 0, 0 }; }
    [[nodiscard]] static constexpr auto green() noexcept -> Color { return Color{ 0, 160, 0 }; }
    [[nodiscard]] static constexpr auto gray() noexcept -> Color { return Color{ 128, 128, 128 }; }
    [[nodiscard]] static constexpr auto yellow() noexcept -> Color { return Color{ 255, 255, 0 }; }
    [[nodiscard]] static constexpr auto transparent() noexcept -> Color { return Color{ 0, 0, 0, 0 }; }

    /// @brief 逐通道相等比较（便于测试/快照断言）。
    [[nodiscard]] constexpr auto operator==(const Color &o) const noexcept -> bool {
        return m_r == o.m_r && m_g == o.m_g && m_b == o.m_b && m_a == o.m_a;
    }
    // NOLINTNEXTLINE(*-redundant-parentheses)
    [[nodiscard]] constexpr auto operator!=(const Color &o) const noexcept -> bool { return !(*this == o); }

    /// @brief 明度缩放：RGB 乘以系数 k（保留 alpha 与色相）。控件 hover/pressed 状态色的
    /// 统一派生方式：k<1 调暗（如 hover ×0.92、pressed ×0.80），k>1 调亮（逐通道饱和到 255）。
    [[nodiscard]] constexpr auto shaded(const float k) const noexcept -> Color {
        const auto mul = [](uint8_t v, float f) -> uint8_t {
            const float x = static_cast<float>(v) * f;
            if (x >= 255.0f) {
                return uint8_t{ 255 };
            }
            if (x <= 0.0f) {
                return uint8_t{ 0 };
            }
            return static_cast<uint8_t>(x);
        };
        return Color{ mul(m_r, k), mul(m_g, k), mul(m_b, k), m_a };
    }

    /// @brief 替换 alpha 通道（RGB 不变）；用于淡色底/选区高亮等半透明派生色。
    [[nodiscard]] constexpr auto with_alpha(uint8_t alpha) const noexcept -> Color {
        return Color{ m_r, m_g, m_b, alpha };
    }
};

/// @brief 调色板命名空间（规格 §2：具名色集中在扁平的 `au::colors` 下，易发现）。
/// 与 `Color::red()` 等静态工厂并存；AI 可任选其一。
namespace colors {
constexpr Color AURORA_WHITE = Color::white();
constexpr Color AURORA_BLACK = Color::black();
constexpr Color AURORA_BLUE = Color::blue();
constexpr Color AURORA_RED = Color::red();
constexpr Color AURORA_GREEN = Color::green();
constexpr Color AURORA_GRAY = Color::gray();
constexpr Color AURORA_YELLOW = Color::yellow();
constexpr Color AURORA_TRANSPARENT = Color::transparent();
} // namespace colors

/**
 * @brief 颜色字面量（规格 §1.6，与 `Color(0xFF,0,0)` 构造互补）。
 *
 * 十六进制按 `0xRRGGBB` / `0xRRGGBBAA` 解释；**仅可在 TU 内显式
 * `using namespace au::literals;` 后使用**，禁止头文件全局 `using`。
 *
 * @code
 *   using namespace au::literals;
 *   auto red   = 0xFF0000_rgb;
 *   auto blueA = 0x0000FFFF_rgba;
 * @endcode
 */
namespace literals {
[[nodiscard]] constexpr auto operator""_rgb(const unsigned long long v) noexcept -> Color {
    return Color{ static_cast<uint8_t>((v >> 16U) & 0xFFU), static_cast<uint8_t>((v >> 8U) & 0xFFU),
                  static_cast<uint8_t>(v & 0xFFU) };
}
[[nodiscard]] constexpr auto operator""_rgba(const unsigned long long v) noexcept -> Color {
    return Color{ static_cast<uint8_t>((v >> 24U) & 0xFFU), static_cast<uint8_t>((v >> 16U) & 0xFFU),
                  static_cast<uint8_t>((v >> 8U) & 0xFFU), static_cast<uint8_t>(v & 0xFFU) };
}
} // namespace literals

} // namespace aurora
