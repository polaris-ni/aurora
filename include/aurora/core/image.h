#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora {

struct Image;  // 前向声明：供 detail::loadImage* 返回类型（完整定义见下方）。

// 由 src/aurora/core/image_stb.cpp（stb_image）提供：解码 PNG/JPG/GIF 等通用格式。
// 由 src/aurora/core/image_svg.cpp 提供：内置 SVG 子集光栅化。
// 由 src/aurora/core/image.cpp 提供：内置未压缩 24 位 BMP 解码。
namespace detail {
[[nodiscard]] auto load_image_stb(const std::vector<std::uint8_t> &buf, std::string_view path) -> Result<Image>;
[[nodiscard]] auto load_image_svg(const std::vector<std::uint8_t> &buf, int target_w, int target_h) -> Result<Image>;
// 解析未压缩 24 位 BMP（BGR，自底向上，行 4 字节对齐）→ RGBA8。供 BmpCodec 与 Image::load 复用。
[[nodiscard]] auto load_bmp(const std::vector<std::uint8_t> &b) -> Result<Image>;
}  // namespace detail

/// @brief 图像资源：像素以 RGBA8 线性存储。
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  ///< RGBA8，长度 = width*height*4

    /// @brief 像素内容摘要（FNV-1a 64 位）：纹理缓存等内容寻址键。
    /// 惰性计算并缓存（const 访问下首次求值，经 mutable 落回本对象）；拷贝携带缓存。
    /// GPU 纹理缓存（`GpuGlRhi`）经此摘要寻址，免除每帧全量哈希重算。
    /// ⚠️ 直接改写 `pixels` 后必须调 `invalidate_content_hash()`，否则摘要过期，
    /// 内容寻址的消费方可能命中旧内容（详见 §9.1 / `specification/03-layout-render.md` §8.7）。
    [[nodiscard]] auto content_hash() const -> std::uint64_t;

    /// @brief 使摘要缓存失效（直接改写 `pixels` 后调用，见 `content_hash` 注释）。
    auto invalidate_content_hash() -> void { content_hash_valid_ = false; }

    /// @brief 从文件加载。
    /// 分发策略：未压缩 24 位 BMP 走内置解码；PNG/JPG/GIF/TGA/HDR 等走 vendored
    /// stb_image 零依赖解码；.svg / 内容嗅探 "<svg" 走内置 SVG 子集光栅化。
    /// 当前实现委托 `aurora::image::ImageCodecRegistry` 统一调度。
    [[nodiscard]] static auto load(std::string_view path) -> Result<Image>;

    /// @brief 按目标尺寸光栅化 SVG（矢量图放大不糊）；target_w/h 为 0 时用固有尺寸。
    /// 非 SVG 文件返回错误。
    [[nodiscard]] static auto load_svg(std::string_view path, int target_w = 0, int target_h = 0) -> Result<Image>;

    // ---- 内部缓存（勿直接读写）----
    // content_hash 惰性求值状态；声明在 pixels 之后，保持既有
    // `Image{.width=…, .height=…, .pixels=…}` 聚合初始化兼容（未列字段取默认值）。
    mutable std::uint64_t content_hash_ = 0;
    mutable bool content_hash_valid_ = false;
};

}  // namespace aurora
