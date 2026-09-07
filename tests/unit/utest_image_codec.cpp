/// 测试类型: unit
/// 目标单元: include/aurora/image/image_codec.h
/// 测试说明: 图片编解码抽象（魔数嗅探、扩展名/名称映射、ImageSource 工厂、选项默认值、注册表兜底报错）单元测试

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "aurora/image/image_codec.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_image_codec {

namespace im = aurora::image;

AURORA_TEST() {
    // ---- 1. 枚举值稳定（Unknown==0、RGBA8==0，供注册表/序列化依赖） ----
    {
        AURORA_TEST_CHECK_EQ(static_cast<int>(im::ImageFormat::Unknown), 0);
        AURORA_TEST_CHECK_EQ(static_cast<int>(im::PixelFormat::RGBA8), 0);
    }

    // ---- 2. format_name：小写可读名 ----
    {
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::PNG) == std::string_view("png"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::JPEG) == std::string_view("jpeg"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::GIF) == std::string_view("gif"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::BMP) == std::string_view("bmp"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::WebP) == std::string_view("webp"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::SVG) == std::string_view("svg"));
        AURORA_TEST_CHECK(im::format_name(im::ImageFormat::Unknown) == std::string_view("unknown"));
    }

    // ---- 3. detect_format：按魔数嗅探（不依赖扩展名） ----
    {
        const std::vector<std::uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(png)) == im::ImageFormat::PNG);

        const std::vector<std::uint8_t> jpg = {0xFF, 0xD8, 0xFF, 0x00};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(jpg)) == im::ImageFormat::JPEG);

        const std::vector<std::uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a'};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(gif)) == im::ImageFormat::GIF);

        const std::vector<std::uint8_t> bmp = {'B', 'M', 0, 0};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(bmp)) == im::ImageFormat::BMP);

        const std::vector<std::uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(webp)) == im::ImageFormat::WebP);

        const std::vector<std::uint8_t> junk = {0x00, 0x01, 0x02, 0x03};
        AURORA_TEST_CHECK(im::detect_format(std::span<const std::uint8_t>(junk)) == im::ImageFormat::Unknown);
    }

    // ---- 4. format_from_path：按扩展名（大小写不敏感）兜底推测 ----
    {
        AURORA_TEST_CHECK(im::format_from_path(std::filesystem::path("a.png")) == im::ImageFormat::PNG);
        AURORA_TEST_CHECK(im::format_from_path(std::filesystem::path("a.JPG")) == im::ImageFormat::JPEG);
        AURORA_TEST_CHECK(im::format_from_path(std::filesystem::path("a.jpeg")) == im::ImageFormat::JPEG);
        AURORA_TEST_CHECK(im::format_from_path(std::filesystem::path("a.svg")) == im::ImageFormat::SVG);
        AURORA_TEST_CHECK(im::format_from_path(std::filesystem::path("a.unknown")) == im::ImageFormat::Unknown);
    }

    // ---- 5. ImageSource 工厂：文件 / 内存两种来源 ----
    {
        const auto fs = im::ImageSource::from_file(std::filesystem::path("x.png"));
        AURORA_TEST_CHECK(fs.kind == im::ImageSource::Kind::File);
        AURORA_TEST_CHECK(fs.path == std::filesystem::path("x.png"));

        std::vector<std::uint8_t> data = {1, 2, 3};
        const auto ms = im::ImageSource::from_memory(data);
        AURORA_TEST_CHECK(ms.kind == im::ImageSource::Kind::Memory);
        AURORA_TEST_CHECK_EQ(ms.memory.size(), std::size_t{3});
    }

    // ---- 6. 选项默认值 ----
    {
        const im::DecodeOptions d;
        AURORA_TEST_CHECK(d.desired_format == im::PixelFormat::RGBA8);
        AURORA_TEST_CHECK(d.preserve_aspect);
        AURORA_TEST_CHECK_FALSE(d.premultiply_alpha);

        const im::EncodeOptions e;
        AURORA_TEST_CHECK(e.format == im::ImageFormat::PNG);
        AURORA_TEST_CHECK_EQ(e.quality, 90);
        AURORA_TEST_CHECK(e.lossless);
    }

    // ---- 7. 注册表单例：已注册内置编解码器；垃圾字节解码报错（不崩溃） ----
    {
        auto &reg = im::ImageCodecRegistry::instance();
        AURORA_TEST_CHECK(!reg.registered().empty());

        const std::vector<std::uint8_t> junk = {0xDE, 0xAD, 0xBE, 0xEF};
        const auto r = reg.decode_memory(std::span<const std::uint8_t>(junk));
        AURORA_TEST_CHECK_FALSE(r.ok());
    }
}

}  // namespace aurora::test_cases::utest_image_codec
