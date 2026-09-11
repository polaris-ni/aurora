#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "aurora/core/color.h"

namespace aurora {

/// @brief 色彩空间标注（D0b）。
///
/// golden 基准的 SSOT 是软件参考路径，其色彩空间恒为 sRGB（逐位确定性红线，见 ROADMAP D1/G2）；
/// Display P3 仅作为**标注 + 转换入口**存在：宽色域内容以 P3 标注，输出到 sRGB 目标
/// （软件 Painter / 8bit 像素缓冲）时经本头文件的矩阵转换，验收为 ±1 LSB 容差单测。
/// @note Thread: thread-safe
/// @note Side-effects: pure
enum class ColorSpace : std::uint8_t {
    SRGB,  ///< sRGB（默认；golden 基准唯一色彩空间）
    DisplayP3,  ///< Display P3（D65 白点 + sRGB 传递曲线，ECMA-386 惯例）
};

/// @brief golden 基准唯一色彩空间（软件参考路径 SSOT，逐位确定性红线；D1/G2 重构约束）。
/// utest_offscreen 的 golden 注记（`colorspace` 字段）与本常量共同守卫该约定。
inline constexpr ColorSpace GOLDEN_COLORSPACE = ColorSpace::SRGB;

/// @brief sRGB 传递曲线解码（8bit → 线性光 [0,1]）。Display P3 与 sRGB 共用此曲线。
[[nodiscard]] inline auto srgb_transfer_decode(const std::uint8_t v) noexcept -> float {
    const float c = static_cast<float>(v) / 255.0F;
    if (c <= 0.04045F) {
        return c / 12.92F;
    }
    return std::pow((c + 0.055F) / 1.055F, 2.4F);
}

/// @brief sRGB 传递曲线编码（线性光 [0,1] → 8bit，四舍五入）。越界值夹取到 [0,255]。
[[nodiscard]] inline auto srgb_transfer_encode(const float linear) noexcept -> std::uint8_t {
    const float c = std::clamp(linear, 0.0F, 1.0F);
    const float e = (c <= 0.0031308F) ? (12.92F * c) : ((1.055F * std::pow(c, 1.0F / 2.4F)) - 0.055F);
    return static_cast<std::uint8_t>(std::lround(e * 255.0F));
}

/// @brief 线性光 sRGB → 线性光 Display P3（D65，行主序 3x3；列和均为 1 → 灰阶恒等）。
inline constexpr float SRGB_TO_P3_LINEAR[3][3] = {
    {0.8224621F, 0.1775380F, 0.0000000F},
    {0.0331941F, 0.9668058F, 0.0000000F},
    {0.0170827F, 0.0723974F, 0.9105199F},
};

/// @brief 线性光 Display P3 → 线性光 sRGB（上矩阵的逆；往返误差 < 1e-5 线性量级）。
inline constexpr float P3_TO_SRGB_LINEAR[3][3] = {
    {1.2249402F, -0.2249402F, 0.0000000F},
    {-0.0420570F, 1.0420570F, 0.0000000F},
    {-0.0196376F, -0.0786361F, 1.0982737F},
};

namespace detail {

/// @brief 线性 RGB 经 3x3 矩阵变换（行主序），逐分量夹取到 [0,1]（宽色域 → 窄色域裁剪）。
inline auto transform_linear(float (&rgb)[3], const float (&m)[3][3]) -> void {
    const float r = rgb[0];
    const float g = rgb[1];
    const float b = rgb[2];
    for (int i = 0; i < 3; ++i) {
        rgb[i] = std::clamp((m[i][0] * r) + (m[i][1] * g) + (m[i][2] * b), 0.0F, 1.0F);
    }
}

}  // namespace detail

/// @brief 色彩空间转换（D0b）：8bit RGBA，以线性光为桥梁，alpha 保留，同空间原样返回。
///
/// 转换链：8bit → 传递曲线解码 → 3x3 线性矩阵（P3↔sRGB，D65）→ 传递曲线编码。
/// 宽色域（P3）超出 sRGB 色域的分量夹取到 [0,1]（8bit 目标的固有约束）。
/// 验收：往返转换 ≤ ±3 LSB（饱和原色附近矩阵行相消 + P3 侧 8bit 量化放大，见 utest_color_space）；
/// sRGB golden 基准路径不经过本转换（GOLDEN_COLORSPACE 恒为 sRGB），逐位确定性不受影响。
/// @note Thread: thread-safe
/// @note Side-effects: pure
[[nodiscard]] inline auto convert_color(const Color &c, const ColorSpace from, const ColorSpace to) -> Color {
    if (from == to) {
        return c;
    }
    float rgb[3] = {srgb_transfer_decode(c.r), srgb_transfer_decode(c.g), srgb_transfer_decode(c.b)};
    if (from == ColorSpace::SRGB && to == ColorSpace::DisplayP3) {
        detail::transform_linear(rgb, SRGB_TO_P3_LINEAR);
    } else {
        detail::transform_linear(rgb, P3_TO_SRGB_LINEAR);
    }
    return Color{srgb_transfer_encode(rgb[0]), srgb_transfer_encode(rgb[1]), srgb_transfer_encode(rgb[2]), c.a};
}

/// @brief sRGB → Display P3 便捷封装。
[[nodiscard]] inline auto srgb_to_display_p3(const Color &c) -> Color {
    return convert_color(c, ColorSpace::SRGB, ColorSpace::DisplayP3);
}

/// @brief Display P3 → sRGB 便捷封装（输出到软件 Painter / 8bit 缓冲的标准路径）。
[[nodiscard]] inline auto display_p3_to_srgb(const Color &c) -> Color {
    return convert_color(c, ColorSpace::DisplayP3, ColorSpace::SRGB);
}

}  // namespace aurora
