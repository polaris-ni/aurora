#include <string_view>

#include "aurora/core/image.h"
#include "aurora/image/image_codec.h"

namespace aurora::image {

namespace {

// stb_image 解码 PNG/JPG/GIF/TGA/HDR 等通用光栅格式（零三方依赖）。
class StbCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "stb"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::Unknown; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        if (h.size() >= 8) {
            // PNG: 89 50 4E 47 0D 0A 1A 0A
            if (h[0] == 0x89 && h[1] == 0x50 && h[2] == 0x4E && h[3] == 0x47 && h[4] == 0x0D && h[5] == 0x0A &&
                h[6] == 0x1A && h[7] == 0x0A) {
                return true;
            }
        }
        if (h.size() >= 3) {
            // JPEG: FF D8 FF
            if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) {
                return true;
            }
            // GIF: 47 49 46 38
            if (h[0] == 'G' && h[1] == 'I' && h[2] == 'F') {
                return true;
            }
            // HDR: #?RADIANCE / #?RGBE
            if (h[0] == '#') {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 字节流转字符串视图属预期类型双关
                const std::string_view head(reinterpret_cast<const char *>(h.data()), h.size() < 10 ? h.size() : 10);
                if (head.starts_with("#?RADIANCE") || head.starts_with("#?RGBE")) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        const std::vector buf(data.begin(), data.end());
        return detail::load_image_stb(buf, {});
    }
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): 工厂函数供 registry.cpp 跨 TU 调用，需外部链接
auto create_stb_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<StbCodec>(); }

} // namespace aurora::image
