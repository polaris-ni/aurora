#pragma once

#include <cstdint>

namespace aurora {

// 以下枚举族整体契约：Thread-safe、pure（无副作用、可枚举），各枚举项见自身 ///< 说明。

/// @brief 文本水平对齐（参考 Flutter TextAlign）。
enum class TextAlign : std::uint8_t {
    Left,  ///< 左对齐
    Right,  ///< 右对齐
    Center,  ///< 居中
    Start,  ///< 依赖书写方向（LTR 同 Left，RTL 同 Right）
    End,  ///< 依赖书写方向（LTR 同 Right，RTL 同 Left）
    Justify,  ///< 两端对齐（最后一行按 Left/Center 处理）
};

/// @brief 文本溢出处理（参考 Flutter TextOverflow）。
enum class TextOverflow : std::uint8_t {
    Clip,  ///< 直接裁切到可见区域
    Ellipsis,  ///< 末尾显示省略号（…）
    Fade,  ///< 渐隐（Painter 不支持时降级为 Clip）
};

/// @brief 书写方向（参考 Flutter TextDirection）。
/// 三条作用路径：
///  1. **shaping**：`TextLayoutOpts::direction` 显式设置（nullopt = 按内容自动 guess，现状），
///     RTL 时 HarfBuzz 把字形反转为视觉序（绘制按数组顺序左→右即为正确视觉序）；
///  2. **caret/命中**：RTL 下逻辑下标 ↔ 视觉位置镜像映射（逻辑首字符在右缘）；
///  3. **对齐**：`TextAlign::Start/End` 按方向解析（RTL: Start=Right，End=Left）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
enum class TextDirection : std::uint8_t {
    LTR,  ///< 从左到右（默认）
    RTL,  ///< 从右到左（阿拉伯语 / 希伯来语等）
};

/// @brief 字重（参考 Flutter FontWeight，枚举值即字重数值 100..900）。
enum class FontWeight : std::uint16_t {
    Thin = 100,  ///< 极细（100）
    ExtraLight = 200,  ///< 特细（200）
    Light = 300,  ///< 细（300）
    Normal = 400,  ///< 常规（400，默认）
    Medium = 500,  ///< 中等（500）
    SemiBold = 600,  ///< 半粗（600）
    Bold = 700,  ///< 加粗（700）
    ExtraBold = 800,  ///< 特粗（800）
    Black = 900,  ///< 极粗（900）
};

/// @brief 字形风格（参考 Flutter FontStyle）。
enum class FontStyle : std::uint8_t {
    Normal,  ///< 正体（直立）
    Italic,  ///< 斜体
};

/// @brief 文本装饰线（参考 Flutter TextDecoration，可按位组合）。
enum class TextDecoration : std::uint8_t {
    None = 0,  ///< 无装饰线（空掩码）
    Underline = 1U << 0U,  ///< 下划线
    Overline = 1U << 1U,  ///< 上划线
    LineThrough = 1U << 2U,  ///< 删除线（贯穿）
};

/// @brief 按位或组合装饰线。
/// @param a 左侧装饰线掩码。
/// @param b 右侧装饰线掩码。
/// @return 二者并集对应的装饰线组合值。
[[nodiscard]] constexpr auto operator|(TextDecoration a, TextDecoration b) noexcept -> TextDecoration {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
    return static_cast<TextDecoration>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @brief 按位与测试装饰线。
/// @param a 左侧装饰线掩码。
/// @param b 右侧装饰线掩码。
/// @return 二者按位与后的掩码值。
[[nodiscard]] constexpr auto operator&(TextDecoration a, TextDecoration b) noexcept -> TextDecoration {
    return static_cast<TextDecoration>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/// @brief 按位或赋值。
/// @param a 左侧装饰线掩码（就地并入 `b`）。
/// @param b 右侧装饰线掩码。
/// @return 更新后的 `a` 引用。
constexpr auto operator|=(TextDecoration &a, TextDecoration b) noexcept -> TextDecoration & {
    a = a | b;
    return a;
}

/// @brief 判断 `flags` 是否包含 `f`。
/// @param flags 已组合的装饰线掩码。
/// @param f 待检测的单个装饰线。
/// @return `flags` 含 `f` 时为 true。
[[nodiscard]] constexpr auto decoration_has(TextDecoration flags, TextDecoration f) noexcept -> bool {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(f)) != 0U;
}

/// @brief 主轴尺寸策略（参考 Flutter MainAxisSize）。
enum class MainAxisSize : std::uint8_t {
    Min,  ///< 取内容最小尺寸（默认）
    Max,  ///< 占满父级可用空间
};

/// @brief 主轴对齐方式（参考 Flutter MainAxisAlignment）。
enum class MainAxisAlignment : std::uint8_t {
    Start,  ///< 靠起点
    Center,  ///< 居中
    End,  ///< 靠终点
    SpaceBetween,  ///< 两端贴边，中间均分
    SpaceAround,  ///< 首尾半间距，中间均分
    SpaceEvenly,  ///< 全均分
};

/// @brief 交叉轴对齐方式（参考 Flutter CrossAxisAlignment）。
///
/// `Baseline` 按子项首行文本基线对齐（仅水平主轴有意义）：无基线的子项按 CSS 式合成基线
/// （自身交叉轴底边）参与，不报错；纵向主轴（Column）下交叉轴是水平的，基线无意义，
/// 按 `Start` 处理并提示一次降级。
enum class CrossAxisAlignment : std::uint8_t {
    Start,  ///< 靠起点
    Center,  ///< 居中
    End,  ///< 靠终点
    Stretch,  ///< 拉伸填满
    Baseline,  ///< 按子项首行文本基线对齐（仅水平主轴有意义，见 specification/03-layout-render.md §3.8）
};

/// @brief Stack 子项尺寸拟合（参考 Flutter StackFit）。
enum class StackFit : std::uint8_t {
    Loose,  ///< 子项按自身约束（默认）
    Expand,  ///< 子项强制填满 Stack 约束
    Passthrough,  ///< Stack 约束直接透传给子项（不施加约束）
};

/// @brief 容器溢出策略（参考 CSS overflow / Flutter ClipBehavior）。
enum class OverflowStrategy : std::uint8_t {
    Visible,  ///< 子内容溢出可见（默认）
    Hidden,  ///< 溢出部分隐藏（裁剪）
    Clip,  ///< 同 Hidden，但保留 hit-test（裁剪视觉但事件穿透溢出区域）
    Scroll,  ///< 溢出部分可滚动（预留，当前等同 Hidden）
};

/// @brief 图片缩放拟合（参考 Flutter BoxFit）。
enum class BoxFit : std::uint8_t {
    Fill,  ///< 拉伸填满（可能变形）
    Contain,  ///< 等比缩放，完整可见
    Cover,  ///< 等比缩放，填满并裁切溢出
    FitWidth,  ///< 宽适配（高度按比例）
    FitHeight,  ///< 高适配（宽度按比例）
    None,  ///< 原始尺寸
    ScaleDown,  ///< 仅当大于容器时等比缩小，否则保持原尺寸
};

/// @brief 鼠标光标形状（参考 Flutter SystemMouseCursors / Win32 LoadCursor 家族）。
/// 由 `Modifier::cursor(...)` 声明在控件上、`Widget::cursor_shape()` 虚钩子提供控件级默认；
/// 事件派发器在悬停链变化时解析出目标形状，经 `Surface::set_cursor` 下发到平台光标。
enum class CursorShape : std::uint8_t {
    Arrow,  ///< 默认箭头
    IBeam,  ///< 文本输入 I 形光标
    PointingHand,  ///< 可点击手型
    ResizeNS,  ///< 上下调整大小
    ResizeEW,  ///< 左右调整大小
    ResizeNWSE,  ///< 主对角线（↘↖）调整大小
    ResizeNESW,  ///< 副对角线（↗↙）调整大小
    Move,  ///< 移动
    Crosshair,  ///< 十字准星
    NotAllowed,  ///< 禁止
    Wait,  ///< 等待/忙碌
};

}  // namespace aurora
