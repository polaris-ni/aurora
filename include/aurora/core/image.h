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
/// @brief 经 vendored stb_image 解码通用位图（PNG/JPG/GIF/TGA/HDR 等）→ RGBA8。
/// @param buf 文件字节缓冲。
/// @param path 原始路径（仅用于错误信息定位）。
/// @return 解码结果：成功为 RGBA8 图像，失败为结构化 Error。
[[nodiscard]] auto load_image_stb(const std::vector<std::uint8_t> &buf, std::string_view path) -> Result<Image>;
/// @brief 内置 SVG 子集全矢量光栅化 → RGBA8（矢量放大不失真）。
/// @param buf SVG 字节缓冲。
/// @param target_w 目标宽；0 时用文档固有尺寸。
/// @param target_h 目标高；0 时用文档固有尺寸。
/// @return 光栅化结果：成功为 RGBA8 图像，失败为结构化 Error。
[[nodiscard]] auto load_image_svg(const std::vector<std::uint8_t> &buf, int target_w, int target_h) -> Result<Image>;
/// @brief 解析未压缩 24 位 BMP（BGR，自底向上，行 4 字节对齐）→ RGBA8。供 BmpCodec 与 Image::load 复用。
/// @param b BMP 字节缓冲。
/// @return 解码结果；非 24 位未压缩变体返回结构化 Error。
[[nodiscard]] auto load_bmp(const std::vector<std::uint8_t> &b) -> Result<Image>;
}  // namespace detail

/// @brief 图像资源：像素以 RGBA8 线性存储。
struct Image {
    int width = 0;  ///< 像素宽（图像固有尺寸）
    int height = 0;  ///< 像素高（图像固有尺寸）
    std::vector<std::uint8_t> pixels;  ///< RGBA8，长度 = width*height*4

    /// @brief 像素内容摘要（FNV-1a 64 位）：纹理缓存等内容寻址键。
    /// 惰性计算并缓存（const 访问下首次求值，经 mutable 落回本对象）；拷贝携带缓存。
    /// GPU 纹理缓存（`GpuGlRhi`）经此摘要寻址，免除每帧全量哈希重算。
    /// ⚠️ 直接改写 `pixels` 后必须调 `invalidate_content_hash()`，否则摘要过期，
    /// 内容寻址的消费方可能命中旧内容（详见 §9.1 / `specification/03-layout-render.md` §8.7）。
    /// @return 64 位 FNV-1a 像素内容摘要（缓存命中时直接返回，不改写内容则稳定）。
    [[nodiscard]] auto content_hash() const -> std::uint64_t;

    /// @brief 使摘要缓存失效（直接改写 `pixels` 后调用，见 `content_hash` 注释）。
    auto invalidate_content_hash() -> void { content_hash_valid_ = false; }

    /// @brief 从文件加载。
    /// 分发策略：未压缩 24 位 BMP 走内置解码；PNG/JPG/GIF/TGA/HDR 等走 vendored
    /// stb_image 零依赖解码；.svg / 内容嗅探 "<svg" 走内置 SVG 子集光栅化。
    /// 当前实现委托 `aurora::image::ImageCodecRegistry` 统一调度。
    /// @param path 图像文件路径（按扩展名与内容嗅探选解码器）。
    /// @return 解码后的 Image（RGBA8）；失败携带结构化 Error。
    [[nodiscard]] static auto load(std::string_view path) -> Result<Image>;

    /// @brief 按目标尺寸光栅化 SVG（矢量图放大不糊）；target_w/h 为 0 时用固有尺寸。
    /// 非 SVG 文件返回错误。
    /// @param path SVG 文件路径。
    /// @param target_w 目标宽；0 时用固有尺寸。
    /// @param target_h 目标高；0 时用固有尺寸。
    /// @return 光栅化后的 Image（RGBA8）；非 SVG 或解码失败返回结构化 Error。
    [[nodiscard]] static auto load_svg(std::string_view path, int target_w = 0, int target_h = 0) -> Result<Image>;

    /// @brief 流式纹理键（GPU 常驻流式通道；软件路径忽略）。
    /// `stream_key != 0` 时 GPU 后端按键寻址固定纹理槽（不走 content_hash 缓存、不参与通用缓存淘汰），
    /// 生命周期跟随产生方（如 `VideoPlayer`），键由产生方保证进程内唯一（0 保留为无效）。
    /// @note 配套 `stream_version` 变化即触发增量 sub-upload——视频 / 大图逐帧更新场景消除每帧纹理
    /// 新建 / PMA 全帧副本 / 淘汰抖动（见 specification/03 §8.7）。
    std::uint64_t stream_key = 0;
    std::uint64_t stream_version = 0;  ///< 流式版本号；相对上次值变化即触发 GPU 增量 sub-upload。

    // NOLINTBEGIN(readability-identifier-naming)
    /// @brief 内部缓存（勿直接读写）：content_hash 惰性求值状态。
    /// 声明在 pixels 之后，保持既有 `Image{.width=…, .height=…, .pixels=…}` 聚合初始化兼容
    /// （未列字段取默认值）；尾部 `_` 是内部缓存标记，但 `Image` 是聚合体——成员必须公开，
    /// 否则 `Image{.pixels=…}` 这类既有初始化写法即失效（POD 数据载体约定，见 CODING_STANDARDS.md §2）。
    /// @note 上方区间式豁免起点与下方终点成对，覆盖两缓存成员的命名豁免。
    mutable std::uint64_t content_hash_ = 0;  ///< 惰性求值的内容摘要缓存值。
    mutable bool content_hash_valid_ = false;  ///< 摘要缓存有效性；改写 pixels 后由 invalidate 置 false。
    // NOLINTEND(readability-identifier-naming)
};

}  // namespace aurora
