#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/result.h"

namespace aurora::icon {

/// @brief SVG 图标光栅请求：源字节 + 目标盒 + 着色。
///
/// 仅走 `load_image_svg` 的内存字节入口（wasm 无文件系统、编解码注册表只支持固有尺寸）。
/// `tint` 为白色（255,255,255,255）时视为不重新着色，直接采用光栅化原色。
/// @note Thread: main-thread only（缓存为进程级单例）
/// @note Side-effects: none（构造期）
struct SvgIconSource {
    std::vector<std::uint8_t> source;  ///< SVG 源字节；空源判定为不支持。
    int target_w = 0;  ///< 目标宽（px）；<=0 由光栅器按固有尺寸。
    int target_h = 0;  ///< 目标高（px）；<=0 由光栅器按固有尺寸。
    Color tint = Color{255, 255, 255, 255};  ///< 着色；白色 = 不重新着色。

    /// @brief 相等比较：四字段全等。
    /// @param o 右操作数。
    /// @return 四字段均相等时为 true。
    [[nodiscard]] auto operator==(const SvgIconSource &o) const -> bool {
        return target_w == o.target_w && target_h == o.target_h && tint == o.tint && source == o.source;
    }
};

/// @brief 缓存键：内容摘要 + 尺寸 + 着色（`SvgIconCache` 私有簿记，置于类前置以供成员声明引用）。
namespace detail {

struct CacheKey {
    std::uint64_t hash = 0;
    int w = 0;
    int h = 0;
    Color tint;
};

/// @brief 缓存键判等。
struct KeyEqual {
    [[nodiscard]] auto operator()(const CacheKey &a, const CacheKey &b) const -> bool {
        return a.hash == b.hash && a.w == b.w && a.h == b.h && a.tint == b.tint;
    }
};

/// @brief 缓存键散列（内容摘要 / 尺寸 / 着色混合折叠）。
struct KeyHash {
    [[nodiscard]] auto operator()(const CacheKey &k) const -> std::size_t {
        std::size_t h = std::hash<std::uint64_t>{}(k.hash);
        h ^= std::hash<int>{}(k.w) + 0x9E3779B9U + (h << 6U) + (h >> 2U);
        h ^= std::hash<int>{}(k.h) + 0x9E3779B9U + (h << 6U) + (h >> 2U);
        h ^= std::hash<std::uint32_t>{}(
                 static_cast<std::uint32_t>(k.tint.r) | (static_cast<std::uint32_t>(k.tint.g) << 8U) |
                 (static_cast<std::uint32_t>(k.tint.b) << 16U) | (static_cast<std::uint32_t>(k.tint.a) << 24U)) +
             0x9E3779B9U + (h << 6U) + (h >> 2U);
        return h;
    }
};

}  // namespace detail

/// @brief SVG 图标光栅缓存：按（内容哈希、目标尺寸、着色）三元组为键的 LRU。
///
/// 不与 `ImageCache` 合并——后者以路径字符串为键、经 `Image::load` 解码（固有尺寸），
/// 既无法表达尺寸与着色维度，合并还会污染其 64 MB 预算口径与命中统计。本缓存在满时按
/// 字节封顶淘汰最久未用项并写诊断日志。
/// @note Thread: main-thread only
/// @note Side-effects: 写进程级缓存单例
class SvgIconCache {
  public:
    /// @brief 默认字节上限：4 MB，独立于 `ImageCache` 的 64 MB 预算。
    static constexpr std::size_t AURORA_ICON_SVG_CACHE_MAX_BYTES = std::size_t{4U} * 1024U * 1024U;

    /// @brief 进程级单例。
    /// @return 缓存单例引用。
    [[nodiscard]] static auto instance() -> SvgIconCache &;

    /// @brief 取光栅化结果：命中则直接复用，未命中则校验白名单后光栅化并入缓存。
    /// @param src 光栅请求（源 / 尺寸 / 着色）。
    /// @return 命中或新光栅化的 `Image`；特性不支持返回 `icon-svg-unsupported`，光栅失败返回其源错误。
    [[nodiscard]] auto get_or_rasterize(const SvgIconSource &src) -> Result<Image>;

    /// @brief 清空全部缓存项。
    auto clear() -> void;

    /// @brief 当前缓存占用的字节数（含 RGBA 像素与簿记开销的近似）。
    /// @return 已占用字节。
    [[nodiscard]] auto size_bytes() const -> std::size_t;

    /// @brief 当前缓存条目数。
    /// @return 条目数。
    [[nodiscard]] auto size() const -> std::size_t;

  private:
    struct Entry {
        Image image;
        std::size_t bytes = 0;
        std::list<detail::CacheKey>::iterator lru_it{};
    };

    SvgIconCache() = default;

    std::list<detail::CacheKey> lru_;
    std::unordered_map<detail::CacheKey, Entry, detail::KeyHash, detail::KeyEqual> map_;
    std::size_t bytes_ = 0;
};

/// @brief 按特性白名单校验 SVG 源：内置光栅化只支持 SVG 子集。
///
/// 拒绝内嵌位图（`<image`）、外部对象（`<foreignObject`）、滤镜（`<filter`）、脚本（`<script`）
/// 等本库光栅器不实现的特性；空源或非法包装同样拒绝。
/// @param src SVG 源字节。
/// @return 通过校验为空 `Result`；不通过返回 `icon-svg-unsupported`。
[[nodiscard]] auto validate_svg_whitelist(std::string_view src) -> Result<void>;

}  // namespace aurora::icon
