#pragma once

#include <cmath>
#include <cstdint>

/// @brief Aurora 根命名空间：库的全部公共类型与自由函数均在此命名空间下（推荐别名 `au`）。
namespace aurora {

/// @brief 8 位每通道 RGBA 颜色。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Color {
    uint8_t r = 0;  ///< 红通道强度（0-255，默认 0）。
    uint8_t g = 0;  ///< 绿通道强度（0-255，默认 0）。
    uint8_t b = 0;  ///< 蓝通道强度（0-255，默认 0）。
    uint8_t a = 255;  ///< alpha 不透明度（0 全透明，255 不透明；默认 255）。

    /// @brief 默认构造：各通道取成员默认值，即不透明黑（0, 0, 0, alpha 255）。
    constexpr Color() noexcept = default;

    /// @brief 四通道构造。
    /// @param r 红通道（0-255）。
    /// @param g 绿通道（0-255）。
    /// @param b 蓝通道（0-255）。
    /// @param a alpha 通道（0-255，默认 255 不透明）。
    constexpr Color(const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a = 255) noexcept
        : r(r), g(g), b(b), a(a) {}
    /// @brief 按 8 位通道值构造颜色（与四通道构造等价的具名工厂）。
    /// @param r 红通道（0-255）。
    /// @param g 绿通道（0-255）。
    /// @param b 蓝通道（0-255）。
    /// @param a alpha 通道（0-255，默认 255 不透明）。
    /// @return 对应通道取值的颜色。
    [[nodiscard]] static constexpr auto from_rgba(const uint8_t r, const uint8_t g, const uint8_t b,
                                                  const uint8_t a = 255) noexcept -> Color {
        return Color{r, g, b, a};
    }
    /// @brief 纯白不透明色。
    /// @return (255, 255, 255, 255) 的颜色。
    [[nodiscard]] static constexpr auto white() noexcept -> Color { return Color{255, 255, 255}; }
    /// @brief 纯黑不透明色。
    /// @return (0, 0, 0, 255) 的颜色。
    [[nodiscard]] static constexpr auto black() noexcept -> Color { return Color{0, 0, 0}; }
    /// @brief 纯蓝色。
    /// @return (0, 0, 255, 255) 的颜色。
    [[nodiscard]] static constexpr auto blue() noexcept -> Color { return Color{0, 0, 255}; }
    /// @brief 纯红色。
    /// @return (255, 0, 0, 255) 的颜色。
    [[nodiscard]] static constexpr auto red() noexcept -> Color { return Color{255, 0, 0}; }
    /// @brief 中绿色（非纯绿：g=160，观感更接近 UI 语义绿）。
    /// @return (0, 160, 0, 255) 的颜色。
    [[nodiscard]] static constexpr auto green() noexcept -> Color { return Color{0, 160, 0}; }
    /// @brief 中性灰。
    /// @return (128, 128, 128, 255) 的颜色。
    [[nodiscard]] static constexpr auto gray() noexcept -> Color { return Color{128, 128, 128}; }
    /// @brief 纯黄色。
    /// @return (255, 255, 0, 255) 的颜色。
    [[nodiscard]] static constexpr auto yellow() noexcept -> Color { return Color{255, 255, 0}; }
    /// @brief 全透明（RGB 无意义，仅作透明占位）。
    /// @return (0, 0, 0, 0) 的颜色。
    [[nodiscard]] static constexpr auto transparent() noexcept -> Color { return Color{0, 0, 0, 0}; }

    /// @brief 逐通道相等比较（便于测试/快照断言）。
    /// @param o 比较对象。
    /// @return 四通道（r/g/b/a）全部相等时为 true。
    [[nodiscard]] constexpr auto operator==(const Color &o) const noexcept -> bool {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    /// @brief 逐通道不等比较（operator== 的取反）。
    /// @param o 比较对象。
    /// @return 任一通道不同则为 true。
    /// NOLINTNEXTLINE(*-redundant-parentheses)
    [[nodiscard]] constexpr auto operator!=(const Color &o) const noexcept -> bool { return !(*this == o); }

    /// @brief 明度缩放：RGB 乘以系数 k（保留 alpha 与色相）。控件 hover/pressed 状态色的
    /// 统一派生方式：k<1 调暗（如 hover ×0.92、pressed ×0.80），k>1 调亮（逐通道饱和到 255）。
    /// @param k 明度系数，逐通道乘以 RGB；乘积越界时钳位到 0~255。
    /// @return RGB 逐通道乘 k 并钳位/截断后的新颜色；alpha 原样保留。
    [[nodiscard]] constexpr auto shaded(const float k) const noexcept -> Color {
        const auto mul = [](uint8_t v, float f) -> uint8_t {
            const float x = static_cast<float>(v) * f;
            if (x >= 255.0F) {
                return uint8_t{255};
            }
            if (x <= 0.0F) {
                return uint8_t{0};
            }
            return static_cast<uint8_t>(x);
        };
        return Color{mul(r, k), mul(g, k), mul(b, k), a};
    }

    /// @brief 替换 alpha 通道（RGB 不变）；用于淡色底/选区高亮等半透明派生色。
    /// @param alpha 新的不透明度（0-255，0 全透明、255 不透明）。
    /// @return RGB 三通道不变、alpha 更新后的颜色。
    [[nodiscard]] constexpr auto with_alpha(uint8_t alpha) const noexcept -> Color { return Color{r, g, b, alpha}; }
};

/// @brief 相对亮度（WCAG 2.x 定义）：sRGB 三通道线性化后的加权和。
///
/// 全库唯一的一份感知亮度算法：无障碍配对断言、色角色对比度门禁与运行期覆盖面校验都取此值，
/// 不得另写第二份（三处各说各话正是本函数要消灭的形态）。
///
/// @param c 输入颜色。**alpha 不参与**——半透明层（如状态叠色）的对比度只在它合成到某个底色之后
///        才有定义，未合成前求出的比值无意义；该口径见 `codespec/specification/07-environment-modifier.md` §5.1。
/// @return 区间 [0, 1] 的相对亮度：纯黑为 0，纯白为 1。
/// @note Thread: thread-safe
/// @note Side-effects: pure
[[nodiscard]] inline auto relative_luminance(const Color &c) noexcept -> double {
    const auto linear = [](const uint8_t v) noexcept -> double {
        const double s = static_cast<double>(v) / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * linear(c.r)) + (0.7152 * linear(c.g)) + (0.0722 * linear(c.b));
}

/// @brief 两色对比度（WCAG 2.x 定义）：较亮者与较暗者的相对亮度商。
///
/// 判定阈值不在本函数里：3:1 / 4.5:1 是「哪一类配对」的属性（非文本 / 文本），见
/// `codespec/specification/07-environment-modifier.md` §5.1 的三档口径与色角色对比度门禁。
/// 公式对参数次序对称，前景与底色谁先传都一样。
///
/// @param a 第一色（alpha 不参与，理由同 `relative_luminance`）。
/// @param b 第二色（alpha 不参与）。
/// @return 区间 [1, 21] 的对比度：同色为 1，纯黑配纯白为 21。
/// @note Thread: thread-safe
/// @note Side-effects: pure
[[nodiscard]] inline auto contrast_ratio(const Color &a, const Color &b) noexcept -> double {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    const double hi = la >= lb ? la : lb;
    const double lo = la >= lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

/// @brief 调色板命名空间（需求 SPEC.API.NAMING-CONSISTENCY.001：具名色集中在扁平的 `au::colors` 下，易发现）。
/// 与 `Color::red()` 等静态工厂并存；AI 可任选其一。
namespace colors {
constexpr Color AURORA_WHITE = Color::white();  ///< 纯白不透明色（= Color::white()）。
constexpr Color AURORA_BLACK = Color::black();  ///< 纯黑不透明色（= Color::black()）。
constexpr Color AURORA_BLUE = Color::blue();  ///< 纯蓝色（= Color::blue()）。
constexpr Color AURORA_RED = Color::red();  ///< 纯红色（= Color::red()）。
constexpr Color AURORA_GREEN = Color::green();  ///< 中绿色 g=160（= Color::green()）。
constexpr Color AURORA_GRAY = Color::gray();  ///< 中性灰（= Color::gray()）。
constexpr Color AURORA_YELLOW = Color::yellow();  ///< 纯黄色（= Color::yellow()）。
constexpr Color AURORA_TRANSPARENT = Color::transparent();  ///< 全透明占位色（= Color::transparent()）。
}  // namespace colors

/// @brief 颜色字面量（需求 SPEC.API.STRONG-TYPES.001，与 `Color(0xFF,0,0)` 构造互补）。
///
/// 十六进制按 `0xRRGGBB` / `0xRRGGBBAA` 解释；**仅可在 TU 内显式
/// `using namespace au::literals;` 后使用**，禁止头文件全局 `using`。
///
/// @code
/// using namespace au::literals;
/// auto red   = 0xFF0000_rgb;
/// auto blueA = 0x0000FFFF_rgba;
/// @endcode
namespace literals {
[[nodiscard]] constexpr auto operator""_rgb(const unsigned long long v) noexcept -> Color {
    return Color{static_cast<uint8_t>((v >> 16U) & 0xFFU), static_cast<uint8_t>((v >> 8U) & 0xFFU),
                 static_cast<uint8_t>(v & 0xFFU)};
}
[[nodiscard]] constexpr auto operator""_rgba(const unsigned long long v) noexcept -> Color {
    return Color{static_cast<uint8_t>((v >> 24U) & 0xFFU), static_cast<uint8_t>((v >> 16U) & 0xFFU),
                 static_cast<uint8_t>((v >> 8U) & 0xFFU), static_cast<uint8_t>(v & 0xFFU)};
}
}  // namespace literals

}  // namespace aurora
