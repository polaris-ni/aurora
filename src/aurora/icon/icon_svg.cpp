#include "aurora/icon/icon_svg.h"

#include <algorithm>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "aurora/core/image.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"

namespace aurora::icon {
namespace {

/// @brief 把 SVG 源字节经 FNV-1a 折叠成 64 位摘要，作为缓存键的内容维。
/// @param src SVG 源字节。
/// @return 64 位内容摘要。
auto source_hash(std::string_view src) -> std::uint64_t {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    std::uint64_t h = fnv_offset;
    for (const unsigned char c : src) {
        h ^= static_cast<std::uint64_t>(c);
        h *= fnv_prime;
    }
    return h;
}

/// @brief 按着色重新着色 RGBA8 像素（直 alpha 假设，与 `Image` 既有像素约定一致）。
/// @param img 待着色图像（就地修改像素）。
/// @param tint 着色；白色 = 不重新着色（直接返回）。
auto apply_tint(Image &img, const Color &tint) -> void {
    if (tint == Color{255, 255, 255, 255}) {
        return;
    }
    const float kr = static_cast<float>(tint.r) / 255.0F;
    const float kg = static_cast<float>(tint.g) / 255.0F;
    const float kb = static_cast<float>(tint.b) / 255.0F;
    const float ka = static_cast<float>(tint.a) / 255.0F;
    std::vector<std::uint8_t> &px = img.pixels;
    const std::size_t n = px.size();
    for (std::size_t i = 0; i + 3U < n; i += 4U) {
        px[i] = static_cast<std::uint8_t>(static_cast<float>(px[i]) * kr);
        px[i + 1U] = static_cast<std::uint8_t>(static_cast<float>(px[i + 1U]) * kg);
        px[i + 2U] = static_cast<std::uint8_t>(static_cast<float>(px[i + 2U]) * kb);
        px[i + 3U] = static_cast<std::uint8_t>(static_cast<float>(px[i + 3U]) * ka);
    }
    img.invalidate_content_hash();
}

}  // namespace

auto validate_svg_whitelist(std::string_view src) -> Result<void> {
    const bool empty = src.empty();
    const bool wraps_svg = src.find("<svg") != std::string_view::npos ||
                           (src.find("<?xml") != std::string_view::npos && src.find("svg") != std::string_view::npos);
    if (empty || !wraps_svg) {
        return make_error(ErrorCode::IconSvgUnsupported,
                          ErrorParams{{"detail", "source is empty or not an SVG document"}});
    }
    // 内置光栅器不实现的特性：内嵌位图 / 外部对象 / 滤镜 / 脚本。
    const char *const unsupported[] = {"<image", "<foreignObject", "<filter", "<script", "<style"};
    for (const char *token : unsupported) {
        if (src.find(token) != std::string_view::npos) {
            return make_error(ErrorCode::IconSvgUnsupported,
                              ErrorParams{{"detail", std::string{"unsupported SVG feature: "} + token}});
        }
    }
    return Result<void>{};
}

auto SvgIconCache::instance() -> SvgIconCache & {
    static SvgIconCache cache;
    return cache;
}

auto SvgIconCache::get_or_rasterize(const SvgIconSource &src) -> Result<Image> {
    detail::CacheKey key{.hash = source_hash(std::string_view{
                             reinterpret_cast<const char *>(src.source.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                             src.source.size()}),
                         .w = src.target_w,
                         .h = src.target_h,
                         .tint = src.tint};
    auto &self = instance();
    // 命中：移到 LRU 表头并复用。
    if (auto hit = self.map_.find(key); hit != self.map_.end()) {
        self.lru_.splice(self.lru_.begin(), self.lru_, hit->second.lru_it);
        return hit->second.image;
    }
    // 未命中：先白名单校验，再光栅化。
    if (const auto v = validate_svg_whitelist(
            std::string_view{reinterpret_cast<const char *>(src.source.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                             src.source.size()});
        !v.ok()) {
        return v.error();
    }
    auto img = aurora::detail::load_image_svg(src.source, src.target_w, src.target_h);
    if (!img.ok()) {
        return img.error();
    }
    apply_tint(img.value(), src.tint);

    Entry e{.image = std::move(img.value()),
            .bytes = (static_cast<std::size_t>(key.w) * static_cast<std::size_t>(key.h) * 4U) + 64U};
    self.lru_.push_front(key);
    e.lru_it = self.lru_.begin();
    const auto [it, ok] = self.map_.emplace(key, std::move(e));
    (void)ok;
    self.bytes_ += it->second.bytes;

    // 超字节上限：从 LRU 表尾淘汰最久未用项。
    while (self.bytes_ > AURORA_ICON_SVG_CACHE_MAX_BYTES && self.lru_.size() > 1U) {
        const detail::CacheKey victim = self.lru_.back();
        const auto vit = self.map_.find(victim);
        if (vit == self.map_.end()) {
            self.lru_.pop_back();
            continue;
        }
        self.bytes_ -= vit->second.bytes;
        self.map_.erase(vit);
        self.lru_.pop_back();
        AURORA_LOG_WARN("icon.svg", "svg icon cache evicted entry (budget exceeded)");
    }
    return it->second.image;
}

auto SvgIconCache::clear() -> void {
    map_.clear();
    lru_.clear();
    bytes_ = 0U;
}

auto SvgIconCache::size_bytes() const -> std::size_t { return bytes_; }

auto SvgIconCache::size() const -> std::size_t { return map_.size(); }

}  // namespace aurora::icon
