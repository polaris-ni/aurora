#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

struct Image; // 前向声明：供 detail::loadImage* 返回类型（完整定义见下方）。

// 由 src/aurora/core/image_stb.cpp（stb_image）提供：解码 PNG/JPG/GIF 等通用格式。
// 由 src/aurora/core/image_svg.cpp 提供：内置 SVG 子集光栅化。
// 由 src/aurora/core/image.cpp 提供：内置未压缩 24 位 BMP 解码。
namespace detail {
[[nodiscard]] auto load_image_stb(const std::vector<std::uint8_t> &buf, std::string_view path) -> Result<Image>;
[[nodiscard]] auto load_image_svg(const std::vector<std::uint8_t> &buf, int target_w, int target_h) -> Result<Image>;
// 解析未压缩 24 位 BMP（BGR，自底向上，行 4 字节对齐）→ RGBA8。供 BmpCodec 与 Image::load 复用。
[[nodiscard]] auto load_bmp(const std::vector<std::uint8_t> &b) -> Result<Image>;
} // namespace detail

/// @brief 图像资源：像素以 RGBA8 线性存储。
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; ///< RGBA8，长度 = width*height*4

    /// @brief 从文件加载。
    /// 分发策略：未压缩 24 位 BMP 走内置解码；PNG/JPG/GIF/TGA/HDR 等走 vendored
    /// stb_image 零依赖解码；.svg / 内容嗅探 "<svg" 走内置 SVG 子集光栅化。
    /// 当前实现委托 `aurora::image::ImageCodecRegistry` 统一调度。
    [[nodiscard]] static auto load(std::string_view path) -> Result<Image>;

    /// @brief 按目标尺寸光栅化 SVG（矢量图放大不糊）；target_w/h 为 0 时用固有尺寸。
    /// 非 SVG 文件返回错误。
    [[nodiscard]] static auto load_svg(std::string_view path, int target_w = 0, int target_h = 0) -> Result<Image>;
};

} // namespace aurora
