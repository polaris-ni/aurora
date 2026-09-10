/// 测试类型: unit
/// 目标单元: include/aurora/image/image_codec.h
/// 测试说明: 覆盖 ImageFormat/PixelFormat 的名称与魔数嗅探（PNG/JPEG/GIF/BMP/WebP/SVG 与未知兜底）、
/// 按扩展名推测、ImageSource 两种来源构造、注册表调度与自定义编解码器接入、
/// 编码→解码往返保真、畸形输入的结构化错误，以及静态图降级为单帧动图

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/image/image_codec.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image_codec {

namespace {
/// @brief 仅供本文件使用的测试编解码器：以私有魔数触发，验证注册表可插拔调度。
class ProbeCodec : public image::ImageCodec {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "probe"; }

    [[nodiscard]] auto format() const -> image::ImageFormat override { return image::ImageFormat::Unknown; }

    [[nodiscard]] auto can_decode() const -> bool override { return true; }
    [[nodiscard]] auto can_encode() const -> bool override { return true; }

    [[nodiscard]] auto sniff(std::span<const std::uint8_t> header) const -> bool override {
        return header.size() >= 4 && header[0] == 'A' && header[1] == 'U' && header[2] == 'R' && header[3] == 'P';
    }

    [[nodiscard]] auto decode(std::span<const std::uint8_t> data,
                              [[maybe_unused]] const image::DecodeOptions& opt) const -> Result<Image> override {
        // 真实编解码器在 decode 内也会校验魔数：注册表的兜底轮会尝试「未嗅探命中」的编解码器，
        // 若此处不校验，任何字节流都会被本 codec 吞下（测试需还原该契约）。
        if (!sniff(data)) {
            return make_error(ErrorCode::IOImageDecodeFailed, std::string(name()) + ": bad magic");
        }
        Image img;
        img.width = 2;
        img.height = 2;
        img.pixels.assign(static_cast<std::size_t>(2 * 2 * 4), 0x7F);
        return img;
    }

    [[nodiscard]] auto encode([[maybe_unused]] const Image& img, [[maybe_unused]] const image::EncodeOptions& opt) const
        -> Result<std::vector<std::uint8_t>> override {
        return std::vector<std::uint8_t>{'A', 'U', 'R', 'P'};
    }
};

[[nodiscard]] auto make_image(int w, int h, std::uint8_t v) -> Image {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, v);
    return img;
}
}  // namespace

AURORA_TEST_CASE(format_names_are_lowercase_and_distinct) {
    AURORA_TEST_CHECK_EQ(image::format_name(image::ImageFormat::BMP), std::string_view{"bmp"});
    AURORA_TEST_CHECK_EQ(image::format_name(image::ImageFormat::PNG), std::string_view{"png"});
    AURORA_TEST_CHECK_EQ(image::format_name(image::ImageFormat::JPEG), std::string_view{"jpeg"});
    AURORA_TEST_CHECK_EQ(image::format_name(image::ImageFormat::SVG), std::string_view{"svg"});
    AURORA_TEST_CHECK_EQ(image::format_name(image::ImageFormat::Unknown), std::string_view{"unknown"});
    AURORA_TEST_CHECK_NE(image::format_name(image::ImageFormat::GIF), image::format_name(image::ImageFormat::WebP));
}

AURORA_TEST_CASE(detect_format_by_magic) {
    const std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const std::vector<std::uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xE0};
    const std::vector<std::uint8_t> gif{'G', 'I', 'F', '8', '9', 'a'};
    const std::vector<std::uint8_t> bmp{'B', 'M', 0x00, 0x00};
    const std::vector<std::uint8_t> svg{'<', 's', 'v', 'g', ' ', '>'};
    const std::vector<std::uint8_t> unknown{0x01, 0x02, 0x03, 0x04};

    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(png)), static_cast<int>(image::ImageFormat::PNG));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(jpeg)), static_cast<int>(image::ImageFormat::JPEG));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(gif)), static_cast<int>(image::ImageFormat::GIF));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(bmp)), static_cast<int>(image::ImageFormat::BMP));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(unknown)),
                         static_cast<int>(image::ImageFormat::Unknown));
}

AURORA_TEST_CASE(svg_has_no_magic_and_is_resolved_by_extension) {
    // SVG 是文本格式、无魔数：嗅探返回 Unknown，须经扩展名（format_from_path）判定。
    const std::vector<std::uint8_t> svg{'<', 's', 'v', 'g', ' ', '>'};
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(svg)), static_cast<int>(image::ImageFormat::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::format_from_path("icon.svg")),
                         static_cast<int>(image::ImageFormat::SVG));
}

AURORA_TEST_CASE(detect_webp_by_riff_container) {
    // WebP: "RIFF" + 4B size + "WEBP"。
    const std::vector<std::uint8_t> webp{'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P'};
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::detect_format(webp)), static_cast<int>(image::ImageFormat::WebP));
}

AURORA_TEST_CASE(format_from_path_uses_extension) {
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::format_from_path("a.png")), static_cast<int>(image::ImageFormat::PNG));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::format_from_path("a.jpg")),
                         static_cast<int>(image::ImageFormat::JPEG));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::format_from_path("a.svg")), static_cast<int>(image::ImageFormat::SVG));
    AURORA_TEST_CHECK_EQ(static_cast<int>(image::format_from_path("a.unknown-ext")),
                         static_cast<int>(image::ImageFormat::Unknown));
}

AURORA_TEST_CASE(image_source_constructors) {
    const auto from_file = image::ImageSource::from_file("some/path.png");
    AURORA_TEST_CHECK_EQ(static_cast<int>(from_file.kind), static_cast<int>(image::ImageSource::Kind::File));
    AURORA_TEST_CHECK_EQ(from_file.path.string(), std::string{"some/path.png"});

    auto from_memory = image::ImageSource::from_memory({1, 2, 3});
    AURORA_TEST_CHECK_EQ(static_cast<int>(from_memory.kind), static_cast<int>(image::ImageSource::Kind::Memory));
    AURORA_TEST_CHECK_EQ(from_memory.memory.size(), 3U);
}

AURORA_TEST_CASE(registry_registers_builtin_codecs) {
    const auto codecs = image::ImageCodecRegistry::instance().registered();
    AURORA_TEST_CHECK_FALSE(codecs.empty());
}

AURORA_TEST_CASE(custom_codec_is_dispatched_by_sniff) {
    image::ImageCodecRegistry::instance().register_codec(std::make_shared<ProbeCodec>());

    const std::vector<std::uint8_t> probe{'A', 'U', 'R', 'P', 0x00};
    const auto decoded = image::ImageCodecRegistry::instance().decode_memory(probe);
    AURORA_TEST_REQUIRE_TRUE(decoded.ok());
    AURORA_TEST_CHECK_EQ(decoded.value().width, 2);
    AURORA_TEST_CHECK_EQ(decoded.value().height, 2);
}

AURORA_TEST_CASE(encode_then_decode_png_roundtrip) {
    const Image src = make_image(4, 3, 0x33);
    const auto encoded = image::ImageCodecRegistry::instance().encode(src, image::EncodeOptions{});
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    const auto decoded = image::ImageCodecRegistry::instance().decode_memory(encoded.value());
    AURORA_TEST_REQUIRE_TRUE(decoded.ok());
    AURORA_TEST_CHECK_EQ(decoded.value().width, 4);
    AURORA_TEST_CHECK_EQ(decoded.value().height, 3);
    AURORA_TEST_CHECK_EQ(decoded.value().pixels.size(), src.pixels.size());
}

AURORA_TEST_CASE(decode_garbage_returns_error_not_crash) {
    const std::vector<std::uint8_t> garbage{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    const auto decoded = image::ImageCodecRegistry::instance().decode_memory(garbage);
    AURORA_TEST_CHECK_FALSE(decoded.ok());
}

AURORA_TEST_CASE(decode_missing_file_returns_error) {
    const auto decoded = image::ImageCodecRegistry::instance().decode_file("no/such/file.png");
    AURORA_TEST_CHECK_FALSE(decoded.ok());
}

AURORA_TEST_CASE(static_image_degrades_to_single_frame_animation) {
    const Image src = make_image(2, 2, 0x10);
    const auto encoded = image::ImageCodecRegistry::instance().encode(src, image::EncodeOptions{});
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    const auto animated = image::ImageCodecRegistry::instance().decode_animated_memory(encoded.value());
    AURORA_TEST_REQUIRE_TRUE(animated.ok());
    AURORA_TEST_CHECK_EQ(animated.value().frames.size(), 1U);
    AURORA_TEST_CHECK_EQ(animated.value().width, 2);
    AURORA_TEST_CHECK_EQ(animated.value().height, 2);
}

AURORA_TEST_CASE(decode_async_resolves_to_same_result) {
    AURORA_TEST_REQUIRE_THREADS();
    const Image src = make_image(3, 3, 0x20);
    const auto encoded = image::ImageCodecRegistry::instance().encode(src, image::EncodeOptions{});
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    auto future = image::ImageCodecRegistry::instance().decode_async(image::ImageSource::from_memory(encoded.value()));
    const auto decoded = future.get();
    AURORA_TEST_REQUIRE_TRUE(decoded.ok());
    AURORA_TEST_CHECK_EQ(decoded.value().width, 3);
}

}  // namespace aurora::test_cases::utest_image_codec
