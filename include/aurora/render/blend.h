#pragma once

#include <cstdint>

namespace aurora {

/// @brief 像素混合模式（对标 CSS `mix-blend-mode` 的常用子集）。
/// 在「已绘制内容」与纯色 `tint` 之间按模式逐通道混合。
enum class BlendMode : std::uint8_t {
    Normal,     ///< 直接覆盖为 tint
    Multiply,   ///< 正片叠底：src * tint / 255
    Screen,     ///< 滤色：255 - (255-src)*(255-tint)/255
    Overlay,    ///< 叠加（保留对比）
    Darken,     ///< 取暗：min(src, tint)
    Lighten,    ///< 取亮：max(src, tint)
    Difference, ///< 差值：|src - tint|
    Exclusion,  ///< 排除：src + tint - 2*src*tint/255
};

/// @brief 着色器遮罩类型：把内容盒像素 RGB 乘以渐变因子，形成淡出 / 聚焦视觉。
enum class ShaderMaskKind : std::uint8_t {
    LinearFade, ///< 顶部不透明 → 底部淡出（沿主轴）
    LinearRise, ///< 顶部透明 → 底部不透明（沿主轴）
    RadialFade, ///< 中心不透明 → 边缘淡出
};

} // namespace aurora
