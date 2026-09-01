#include "aurora/image/image_codec.h"
#include "aurora/render/png.h"

namespace aurora::image {

namespace {

// 内置最小化 PNG 编码器（RGBA8 → PNG 字节，零三方依赖）。仅编码，不解码。
class PngWriteCodec : public ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "png-write"; }
    [[nodiscard]] auto format() const -> ImageFormat override { return ImageFormat::PNG; }
    [[nodiscard]] auto can_encode() const -> bool override { return true; }
    [[nodiscard]] auto can_decode() const -> bool override { return false; }

    [[nodiscard]] auto encode(const Image &img, const EncodeOptions & /*opt*/) const
        -> Result<std::vector<std::uint8_t>> override {
        if (img.pixels.empty() || img.width <= 0 || img.height <= 0) {
            return make_error(ErrorCode::LayoutInvalidConstraints, "PngWriteCodec: empty image");
        }
        return detail::write_png_to_memory(img.pixels.data(), img.width, img.height);
    }
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): 工厂函数供 registry.cpp 跨 TU 调用，需外部链接
auto create_png_write_codec() -> std::shared_ptr<ImageCodec> { return std::make_shared<PngWriteCodec>(); }

} // namespace aurora::image
