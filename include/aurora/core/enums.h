#pragma once

#include <cstdint>

namespace aurora {

/// @note Thread: thread-safe
/// @note Side-effects: pure
/// @{

/// @brief 文本水平对齐（参考 Flutter TextAlign）。
enum class TextAlign : std::uint8_t {
    Left,
    Right,
    Center,
    Start,   ///< 依赖书写方向（LTR 同 Left，RTL 同 Right）
    End,     ///< 依赖书写方向（LTR 同 Right，RTL 同 Left）
    Justify, ///< 两端对齐（最后一行按 Left/Center 处理）
};

/// @brief 文本溢出处理（参考 Flutter TextOverflow）。
enum class TextOverflow : std::uint8_t {
    Clip,     ///< 直接裁切到可见区域
    Ellipsis, ///< 末尾显示省略号（…）
    Fade,     ///< 渐隐（Painter 不支持时降级为 Clip）
};

/// @brief 字重（参考 Flutter FontWeight，枚举值即字重数值 100..900）。
enum class FontWeight : std::uint16_t {
    Thin = 100,
    ExtraLight = 200,
    Light = 300,
    Normal = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
    ExtraBold = 800,
    Black = 900,
};

/// @brief 字形风格（参考 Flutter FontStyle）。
enum class FontStyle : std::uint8_t {
    Normal,
    Italic,
};

/// @brief 文本装饰线（参考 Flutter TextDecoration，可按位组合）。
enum class TextDecoration : std::uint8_t {
    None = 0,
    Underline = 1u << 0u,
    Overline = 1u << 1u,
    LineThrough = 1u << 2u,
};

/// @brief 按位或组合装饰线。
[[nodiscard]] constexpr auto operator|(TextDecoration a, TextDecoration b) noexcept -> TextDecoration {
    return static_cast<TextDecoration>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @brief 按位与测试装饰线。
[[nodiscard]] constexpr auto operator&(TextDecoration a, TextDecoration b) noexcept -> TextDecoration {
    return static_cast<TextDecoration>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/// @brief 按位或赋值。
constexpr auto operator|=(TextDecoration &a, TextDecoration b) noexcept -> TextDecoration & {
    a = a | b;
    return a;
}

/// @brief 判断 `flags` 是否包含 `f`。
[[nodiscard]] constexpr auto decoration_has(TextDecoration flags, TextDecoration f) noexcept -> bool {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(f)) != 0u;
}

/// @brief 主轴尺寸策略（参考 Flutter MainAxisSize）。
enum class MainAxisSize : std::uint8_t {
    Min, ///< 取内容最小尺寸（默认）
    Max, ///< 占满父级可用空间
};

/// @brief 主轴对齐方式（参考 Flutter MainAxisAlignment）。
enum class MainAxisAlignment : std::uint8_t {
    Start,        ///< 靠起点
    Center,       ///< 居中
    End,          ///< 靠终点
    SpaceBetween, ///< 两端贴边，中间均分
    SpaceAround,  ///< 首尾半间距，中间均分
    SpaceEvenly,  ///< 全均分
};

/// @brief 交叉轴对齐方式（参考 Flutter CrossAxisAlignment）。
enum class CrossAxisAlignment : std::uint8_t {
    Start,   ///< 靠起点
    Center,  ///< 居中
    End,     ///< 靠终点
    Stretch, ///< 拉伸填满
};

/// @brief Stack 子项尺寸拟合（参考 Flutter StackFit）。
enum class StackFit : std::uint8_t {
    Loose,       ///< 子项按自身约束（默认）
    Expand,      ///< 子项强制填满 Stack 约束
    Passthrough, ///< Stack 约束直接透传给子项（不施加约束）
};

/// @brief 容器溢出策略（参考 CSS overflow / Flutter ClipBehavior）。
enum class OverflowStrategy : std::uint8_t {
    Visible, ///< 子内容溢出可见（默认）
    Hidden,  ///< 溢出部分隐藏（裁剪）
    Clip,    ///< 同 Hidden，但保留 hit-test（裁剪视觉但事件穿透溢出区域）
    Scroll,  ///< 溢出部分可滚动（预留，当前等同 Hidden）
};

/// @brief 图片缩放拟合（参考 Flutter BoxFit）。
enum class BoxFit : std::uint8_t {
    Fill,      ///< 拉伸填满（可能变形）
    Contain,   ///< 等比缩放，完整可见
    Cover,     ///< 等比缩放，填满并裁切溢出
    FitWidth,  ///< 宽适配（高度按比例）
    FitHeight, ///< 高适配（宽度按比例）
    None,      ///< 原始尺寸
    ScaleDown, ///< 仅当大于容器时等比缩小，否则保持原尺寸
};

/// @} // thread-safe, pure

} // namespace aurora
