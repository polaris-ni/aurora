#include <string_view>

#include "aurora/core/image.h"
#include "aurora/image/image_codec.h"

namespace aurora::image {

namespace {

// 内置 SVG 子集光栅化（仅解码，target=0 → 固有尺寸）。
class SvgCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "svg"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::SVG; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 字节流转字符串视图属预期类型双关
        const std::string_view head(reinterpret_cast<const char *>(h.data()), h.size());
        return head.find("<svg") != std::string_view::npos;
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        const std::vector buf(data.begin(), data.end());
        // 内置 SVG 子集仅支持固有尺寸光栅化；目标尺寸请走 Image::load_svg。
        return detail::load_image_svg(buf, 0, 0);
    }
};

}  // namespace

auto create_svg_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<SvgCodec>(); }

}  // namespace aurora::image
