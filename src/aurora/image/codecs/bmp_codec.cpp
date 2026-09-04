#include "aurora/core/image.h"
#include "aurora/image/image_codec.h"

namespace aurora::image {

namespace {

// 内置未压缩 24 位 BMP 解码（BGR，自底向上，行 4 字节对齐）→ RGBA8。
class BmpCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "bmp"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::BMP; }
    [[nodiscard]] auto can_decode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> h) const -> bool override {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        return h.size() >= 2 && h[0] == 'B' && h[1] == 'M';
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        return detail::load_bmp(std::vector(data.begin(), data.end()));
    }
};

}  // namespace

auto create_bmp_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<BmpCodec>(); }

}  // namespace aurora::image