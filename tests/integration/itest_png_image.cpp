/// 测试类型: integration
/// 目标单元: include/aurora/render/png.h
/// 测试说明: 内置 PNG 编码器（write_png）与 Image::load 解码链路集成——4x4 RGBA 编码写入
///           用例临时目录后解码往返保真（尺寸与像素值）、零尺寸/空指针输入返回结构化错误

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/render/png.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_png_image {

namespace {

/// 临时目录下的 PNG 文件路径。
auto temp_png(const std::string &name) -> std::string {
    const std::filesystem::path dir = aurora::testing::isolation::temp_dir();
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

}  // namespace

AURORA_TEST_CASE(png_roundtrip_preserves_dimensions_and_pixels) {
    constexpr int w = 4;
    constexpr int h = 4;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i) {
        px[(static_cast<std::size_t>(i) * 4) + 0] = 255;  // 红
        px[(static_cast<std::size_t>(i) * 4) + 3] = 255;  // 不透明
    }

    const std::string path = temp_png("png_test.png");
    const auto wr = aurora::write_png(path.c_str(), w, h, px.data());
    AURORA_TEST_CHECK_MSG(wr.ok(), "write_png: encodes 4x4 RGBA");

    const auto lr = aurora::Image::load(path);
    AURORA_TEST_CHECK_MSG(lr.ok(), "Image::load: decodes written PNG");
    const aurora::Image &img = lr.value();
    AURORA_TEST_CHECK_MSG(img.width == w && img.height == h, "Image::load: dimensions preserved");
    AURORA_TEST_CHECK_MSG(img.pixels[0] == 255 && img.pixels[3] == 255, "Image::load: pixel values preserved");
}

AURORA_TEST_CASE(write_png_rejects_zero_dimensions) {
    // 零尺寸 / 空指针：返回结构化错误而非崩溃。
    const std::string path = temp_png("png_bad.png");
    const auto bad = aurora::write_png(path.c_str(), 0, 0, nullptr);
    AURORA_TEST_CHECK_MSG(!bad.ok(), "write_png: zero dimensions returns error");
}

}  // namespace aurora::test_cases::itest_png_image
