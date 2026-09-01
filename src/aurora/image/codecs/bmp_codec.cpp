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
        return h.size() >= 2 && h[0] == 'B' && h[1] == 'M';
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data, const DecodeOptions & /*opt*/) const
        -> Result<Image> override {
        return detail::load_bmp(std::vector(data.begin(), data.end()));
    }
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): 工厂函数供 registry.cpp 跨 TU 调用，需外部链接
auto create_bmp_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<BmpCodec>(); }

} // namespace aurora::image
