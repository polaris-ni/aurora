/// 测试类型: unit
/// 目标单元: include/aurora/core/image.h
/// 测试说明: 覆盖 Image 默认不变量、缺文件错误路径、内置 24 位 BMP
/// 解码往返（BGR→RGBA8、自底向上、行对齐）与畸形输入拒绝、SVG 错误路径

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image {

namespace {

/// @brief 构造最小 2×2 未压缩 24 位 BMP（54 字节头 + 2 行 × 8 字节行距，行 4 字节对齐）。
/// 像素自底向上：文件第 0 行 = 图像第 1 行（蓝、白），文件第 1 行 = 图像第 0 行（红、绿）。
[[nodiscard]] auto make_2x2_bmp() -> std::vector<std::uint8_t> {
    return {
        'B',
        'M',  // 魔数
        70U,
        0U,
        0U,
        0U,  // 文件大小 = 54 + 2*8
        0U,
        0U,
        0U,
        0U,  // 保留字段
        54U,
        0U,
        0U,
        0U,  // 像素数据偏移
        40U,
        0U,
        0U,
        0U,  // BITMAPINFOHEADER 大小
        2U,
        0U,
        0U,
        0U,  // 宽 = 2
        2U,
        0U,
        0U,
        0U,  // 高 = 2（正数 = 自底向上）
        1U,
        0U,  // 平面数
        24U,
        0U,  // 位深 = 24
        0U,
        0U,
        0U,
        0U,  // 压缩 = BI_RGB（未压缩）
        16U,
        0U,
        0U,
        0U,  // 像素数据大小
        0U,
        0U,
        0U,
        0U,  // 水平分辨率
        0U,
        0U,
        0U,
        0U,  // 垂直分辨率
        0U,
        0U,
        0U,
        0U,  // 调色板颜色数
        0U,
        0U,
        0U,
        0U,  // 重要颜色数
        // 文件第 0 行（图像第 1 行）：蓝 (B,G,R)=(255,0,0)、白 (255,255,255)、对齐填充 2 字节
        255U,
        0U,
        0U,
        255U,
        255U,
        255U,
        0U,
        0U,
        // 文件第 1 行（图像第 0 行）：红 (0,0,255)、绿 (0,255,0)、对齐填充 2 字节
        0U,
        0U,
        255U,
        0U,
        255U,
        0U,
        0U,
        0U,
    };
}

/// @brief 在用例唯一临时目录下建独立子目录并写入字节文件（用例结束由调用方 remove_all）。
[[nodiscard]] auto write_temp_file(const std::string& dir_name, const std::string& file_name,
                                   const std::vector<std::uint8_t>& bytes) -> std::filesystem::path {
    const auto dir = std::filesystem::temp_directory_path() / dir_name;
    std::filesystem::create_directories(dir);
    const auto file = dir / file_name;
    std::ofstream out{file, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(*-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(bytes.size()));
    return file;
}

}  // namespace

AURORA_TEST_CASE(default_image_is_empty) {
    // 构造不变量：零尺寸、空像素缓冲（长度契约 = width*height*4 = 0）。
    const aurora::Image image;
    AURORA_TEST_CHECK_EQ(image.width, 0);
    AURORA_TEST_CHECK_EQ(image.height, 0);
    AURORA_TEST_CHECK(image.pixels.empty());
}

AURORA_TEST_CASE(load_missing_file_returns_error) {
    namespace m = aurora::testing::matchers;

    const auto dir = std::filesystem::temp_directory_path() / "aurora_utest_image_missing";
    std::filesystem::create_directories(dir);
    const auto result = aurora::Image::load((dir / "no_such_file.bmp").string());
    AURORA_TEST_CHECK(!result.ok());
    AURORA_TEST_CHECK_THAT(result.error().message, m::has_substr("cannot open"));
    std::filesystem::remove_all(dir);
}

AURORA_TEST_CASE(load_bmp_roundtrip_decodes_rgba8) {
    // 走 Image::load → 编解码注册表 → 内置 BMP 解码的完整链路。
    const auto file = write_temp_file("aurora_utest_image_bmp", "probe_2x2.bmp", make_2x2_bmp());
    const auto result = aurora::Image::load(file.string());
    AURORA_TEST_REQUIRE(result.ok());

    const auto& image = result.value();
    AURORA_TEST_CHECK_EQ(image.width, 2);
    AURORA_TEST_CHECK_EQ(image.height, 2);
    AURORA_TEST_CHECK_EQ(image.pixels.size(), 16U);  // 2*2*4

    // BGR → RGBA8 且行序自底向上：(0,0)=红、(1,0)=绿、(0,1)=蓝、(1,1)=白，A 恒 255。
    AURORA_TEST_CHECK_EQ(image.pixels[0], 255U);  // (0,0) R
    AURORA_TEST_CHECK_EQ(image.pixels[1], 0U);  // (0,0) G
    AURORA_TEST_CHECK_EQ(image.pixels[2], 0U);  // (0,0) B
    AURORA_TEST_CHECK_EQ(image.pixels[3], 255U);  // (0,0) A
    AURORA_TEST_CHECK_EQ(image.pixels[4], 0U);  // (1,0) R
    AURORA_TEST_CHECK_EQ(image.pixels[5], 255U);  // (1,0) G
    AURORA_TEST_CHECK_EQ(image.pixels[6], 0U);  // (1,0) B
    AURORA_TEST_CHECK_EQ(image.pixels[8], 0U);  // (0,1) R
    AURORA_TEST_CHECK_EQ(image.pixels[9], 0U);  // (0,1) G
    AURORA_TEST_CHECK_EQ(image.pixels[10], 255U);  // (0,1) B
    AURORA_TEST_CHECK_EQ(image.pixels[12], 255U);  // (1,1) R
    AURORA_TEST_CHECK_EQ(image.pixels[13], 255U);  // (1,1) G
    AURORA_TEST_CHECK_EQ(image.pixels[14], 255U);  // (1,1) B
    AURORA_TEST_CHECK_EQ(image.pixels[15], 255U);  // (1,1) A

    std::filesystem::remove_all(file.parent_path());
}

AURORA_TEST_CASE(load_bmp_rejects_truncated_pixel_data) {
    // 安全契约：像素区被截断（声明尺寸需要的字节数超出文件）必须拒绝，不得越界读。
    auto truncated = make_2x2_bmp();
    truncated.resize(58U);  // 只保留 4 字节像素区（需要 16）
    const auto result = aurora::detail::load_bmp(truncated);
    AURORA_TEST_CHECK(!result.ok());
}

AURORA_TEST_CASE(load_bmp_rejects_bad_magic_and_tiny_buffer) {
    // 魔数错误 / 缓冲过短：均返回错误而非崩溃。
    auto wrong_magic = make_2x2_bmp();
    wrong_magic[0] = 'X';
    wrong_magic[1] = 'Y';
    AURORA_TEST_CHECK(!aurora::detail::load_bmp(wrong_magic).ok());

    AURORA_TEST_CHECK(!aurora::detail::load_bmp({}).ok());
    AURORA_TEST_CHECK(!aurora::detail::load_bmp({{'B', 'M'}}).ok());
}

AURORA_TEST_CASE(load_bmp_rejects_non_24bit_or_compressed) {
    // 头文件契约：仅支持「未压缩 24 位」；32 位变体按错误行距假设会越界读，必须拒绝。
    auto bmp32 = make_2x2_bmp();
    bmp32[28] = 32U;  // BITMAPINFOHEADER biBitCount 偏移 28
    AURORA_TEST_CHECK(!aurora::detail::load_bmp(bmp32).ok());

    auto compressed = make_2x2_bmp();
    compressed[30] = 1U;  // biCompression = BI_RLE8（偏移 30）
    AURORA_TEST_CHECK(!aurora::detail::load_bmp(compressed).ok());
}

AURORA_TEST_CASE(load_svg_error_paths) {
    // 缺文件：结构化错误返回。
    const auto dir = std::filesystem::temp_directory_path() / "aurora_utest_image_svg";
    std::filesystem::create_directories(dir);
    const auto missing = aurora::Image::load_svg((dir / "no_such.svg").string());
    AURORA_TEST_CHECK(!missing.ok());

    // 非 SVG 内容（无 "<svg" 嗅探标记 / 无法解析）：拒绝。
    const auto garbage =
        write_temp_file("aurora_utest_image_svg", "garbage.svg", {'n', 'o', 't', '-', 'a', '-', 's', 'v', 'g'});
    const auto parsed = aurora::Image::load_svg(garbage.string());
    AURORA_TEST_CHECK(!parsed.ok());

    std::filesystem::remove_all(dir);
}

}  // namespace aurora::test_cases::utest_image
