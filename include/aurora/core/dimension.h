#pragma once

#include <string>

namespace aurora {

/**
 * @brief 强类型尺寸意图工厂（需求 #4 强类型 + 单位标注）。
 *
 * 设计目标：让 `width` / `height` 等只接受带单位的 `Length`，**裸整数在编译期即被拒绝**
 * （`Length` 没有从 `int` 的隐式构造），避免 `button.width(120)` 这类静默单位错误。
 *
 * 用法：
 * @code
 *   au::Text("Hi").width(au::px(120));      // 固定 120 逻辑像素
 *   au::Column{}.height(au::fill());         // 填满父级（对应 Expand）
 *   au::Text("Hi").width(au::percent(0.8f)); // 占父级 80%
 * @endcode
 *
 * 注意：裸 `int`/裸 `float` 无法隐式转为 `Length`，下列写法**编译失败**（正是我们想要的）：
 *   button.width(120);   // 错误：无 Length(int) 转换
 */
[[nodiscard]] constexpr auto px(float v) noexcept -> Length { return Length::fixed(v); }

/// @brief 与设备无关的"密度无关像素"，当前等价于逻辑像素（规格建议保留 dp 概念）。
[[nodiscard]] constexpr auto dp(float v) noexcept -> Length { return Length::fixed(v); }

/// @brief 占父级尺寸的比例 (0~1)。
[[nodiscard]] constexpr auto percent(float fraction) noexcept -> Length { return Length::ratio(fraction); }

/// @brief 填满父级可用空间（对应安卓 match_parent / Flutter Expand）。
[[nodiscard]] constexpr auto fill() noexcept -> Length { return Length::expand(); }

/// @brief 内容自适应（默认；对应安卓 wrap_content）。
[[nodiscard]] constexpr auto auto_length() noexcept -> Length { return Length::wrap(); }

/// @brief 调试用：把尺寸意图渲染为可读字符串（结构快照/日志）。
[[nodiscard]] inline auto to_string(Length len) -> std::string {
    switch (len.kind) {
        case LengthKind::WrapContent:
            return "auto";
        case LengthKind::Expand:
            return "fill";
        case LengthKind::Fixed:
            return "px(" + std::to_string(len.value) + ")";
        case LengthKind::Fraction:
            return "percent(" + std::to_string(len.value) + ")";
    }
    return "auto";
}

/**
 * @brief 尺寸字面量（需求 #4，与 `au::dp`/`au::px` 工厂互补）。
 *
 * **安全模式**：仅允许在 TU 内显式 `using namespace au::literals;` 后使用，
 * **禁止在头文件中全局 `using`**，以免字面量污染所有包含者的命名空间。
 *
 * @code
 *   using namespace au::literals;
 *   auto w = 120_dp;   // 等价于 au::dp(120)
 *   auto x = 8_px;
 * @endcode
 */
namespace literals {
[[nodiscard]] constexpr auto operator""_dp(long double v) noexcept -> Length { return dp(static_cast<float>(v)); }
[[nodiscard]] constexpr auto operator""_dp(unsigned long long v) noexcept -> Length {
    return dp(static_cast<float>(v));
}
[[nodiscard]] constexpr auto operator""_px(long double v) noexcept -> Length { return px(static_cast<float>(v)); }
[[nodiscard]] constexpr auto operator""_px(unsigned long long v) noexcept -> Length {
    return px(static_cast<float>(v));
}
}  // namespace literals

}  // namespace aurora
