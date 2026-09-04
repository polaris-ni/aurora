#include "aurora/image/image_codec.h"

namespace aurora::image {

auto decode_file(const std::filesystem::path &p, const DecodeOptions &opt) -> Result<Image> {
    return ImageCodecRegistry::instance().decode_file(p, opt);
}

auto decode_memory(std::span<const std::uint8_t> data, const DecodeOptions &opt) -> Result<Image> {
    return ImageCodecRegistry::instance().decode_memory(data, opt);
}

auto decode(const ImageSource &src, const DecodeOptions &opt) -> Result<Image> {
    return ImageCodecRegistry::instance().decode(src, opt);
}

auto decode_animated_file(const std::filesystem::path &p, const DecodeOptions &opt) -> Result<AnimatedImage> {
    return ImageCodecRegistry::instance().decode_animated_file(p, opt);
}

auto encode(const Image &img, const EncodeOptions &opt) -> Result<std::vector<std::uint8_t>> {
    return ImageCodecRegistry::instance().encode(img, opt);
}

auto save(const Image &img, const std::filesystem::path &p, const EncodeOptions &opt) -> Result<bool> {
    return ImageCodecRegistry::instance().save(img, p, opt);
}

auto decode_async(const ImageSource &src, const DecodeOptions &opt) -> std::future<Result<Image>> {
    return ImageCodecRegistry::instance().decode_async(src, opt);
}

}  // namespace aurora::image
