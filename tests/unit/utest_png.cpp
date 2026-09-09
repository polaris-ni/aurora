/// 测试类型: unit
/// 目标单元: include/aurora/render/png.h
/// 测试说明: 覆盖内置 PNG 编码器的参数校验、签名与 IHDR 大端宽高写入、IEND 收尾、同输入编码确定性、
/// 写文件成功路径与不可写路径的错误返回，以及 编码→解码 往返的像素保真

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/render/png.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_png {

namespace {
[[nodiscard]] auto solid_rgba(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++i) {
        px[(i * 4U) + 0U] = r;
        px[(i * 4U) + 1U] = g;
        px[(i * 4U) + 2U] = b;
        px[(i * 4U) + 3U] = 255;
    }
    return px;
}
}  // namespace

AURORA_TEST_CASE(encode_rejects_invalid_dimensions) {
    const auto pixels = solid_rgba(2, 2, 1, 2, 3);
    AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(pixels.data(), 0, 2).ok());
    AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(pixels.data(), 2, -1).ok());
    AURORA_TEST_CHECK_FALSE(detail::write_png_to_memory(nullptr, 2, 2).ok());
}

AURORA_TEST_CASE(encoded_stream_starts_with_png_signature) {
    constexpr std::array<std::uint8_t, 8> expected_signature = {137, 80, 78, 71, 13, 10, 26, 10};
    const auto pixels = solid_rgba(1, 1, 255, 0, 0);
    const auto encoded = detail::write_png_to_memory(pixels.data(), 1, 1);

    AURORA_TEST_REQUIRE_TRUE(encoded.ok());
    AURORA_TEST_REQUIRE_GE(encoded.value().size(), expected_signature.size());
    for (std::size_t i = 0; i < expected_signature.size(); ++i) {
        AURORA_TEST_CHECK_EQ(static_cast<int>(encoded.value().at(i)),
                             static_cast<int>(expected_signature.at(i)));
    }
}

AURORA_TEST_CASE(ihdr_carries_big_endian_dimensions) {
    // IHDR: [0..3] 长度 13 / [4..7] "IHDR" / [8..11] 宽 / [12..15] 高 / [16] 位深 8 / [17] 颜色类型 6(RGBA)
    const auto pixels = solid_rgba(3, 5, 0, 0, 0);
    const auto encoded = detail::write_png_to_memory(pixels.data(), 3, 5);
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    const auto &bytes = encoded.value();
    AURORA_TEST_REQUIRE_GE(bytes.size(), 26U);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[12]), 'I');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[13]), 'H');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[14]), 'D');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[15]), 'R');

    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[16]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[17]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[18]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[19]), 3);  // width = 3
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[20]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[21]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[22]), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[23]), 5);  // height = 5
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[24]), 8);  // bit depth
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[25]), 6);  // color type RGBA
}

AURORA_TEST_CASE(encoded_stream_ends_with_iend) {
    const auto pixels = solid_rgba(2, 2, 9, 9, 9);
    const auto encoded = detail::write_png_to_memory(pixels.data(), 2, 2);
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    const auto &bytes = encoded.value();
    AURORA_TEST_REQUIRE_GE(bytes.size(), 12U);
    const std::size_t tail = bytes.size() - 12;  // 长度(4) + "IEND"(4) + CRC(4)
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[tail + 4]), 'I');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[tail + 5]), 'E');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[tail + 6]), 'N');
    AURORA_TEST_CHECK_EQ(static_cast<int>(bytes[tail + 7]), 'D');
}

AURORA_TEST_CASE(encoding_is_deterministic) {
    // 同输入必得同输出：golden 比对的前提（无时间戳、无随机 ID）。
    const auto pixels = solid_rgba(4, 3, 12, 34, 56);
    const auto first = detail::write_png_to_memory(pixels.data(), 4, 3);
    const auto second = detail::write_png_to_memory(pixels.data(), 4, 3);
    AURORA_TEST_REQUIRE_TRUE(first.ok());
    AURORA_TEST_REQUIRE_TRUE(second.ok());
    AURORA_TEST_CHECK_EQ(first.value(), second.value());
}

AURORA_TEST_CASE(write_png_persists_exact_bytes) {
    const auto pixels = solid_rgba(4, 4, 200, 100, 50);
    const auto encoded = detail::write_png_to_memory(pixels.data(), 4, 4);
    AURORA_TEST_REQUIRE_TRUE(encoded.ok());

    const std::filesystem::path out = std::filesystem::path(testing::isolation::temp_dir()) / "solid.png";
    const auto written = write_png(out.string().c_str(), 4, 4, pixels.data());
    AURORA_TEST_REQUIRE_TRUE(written.ok());

    AURORA_TEST_CHECK_TRUE(std::filesystem::exists(out));
    AURORA_TEST_CHECK_EQ(std::filesystem::file_size(out), encoded.value().size());
}

AURORA_TEST_CASE(write_png_fails_on_unwritable_path) {
    // 目标目录不存在 → 结构化错误，不抛异常、不崩。
    const auto pixels = solid_rgba(2, 2, 1, 1, 1);
    const std::filesystem::path out =
        std::filesystem::path(testing::isolation::temp_dir()) / "no-such-dir" / "x.png";
    const auto written = write_png(out.string().c_str(), 2, 2, pixels.data());
    AURORA_TEST_CHECK_FALSE(written.ok());
}

AURORA_TEST_CASE(encode_decode_roundtrip_preserves_pixels) {
    // 编码器输出须能被通用解码器读回（stored-block deflate + adler32 校验）。
    const auto pixels = solid_rgba(6, 4, 10, 20, 30);
    const std::filesystem::path out = std::filesystem::path(testing::isolation::temp_dir()) / "roundtrip.png";
    AURORA_TEST_REQUIRE_TRUE(write_png(out.string().c_str(), 6, 4, pixels.data()).ok());

    const auto decoded = Image::load(out.string());
    AURORA_TEST_REQUIRE_TRUE(decoded.ok());
    AURORA_TEST_CHECK_EQ(decoded.value().width, 6);
    AURORA_TEST_CHECK_EQ(decoded.value().height, 4);
    AURORA_TEST_REQUIRE_EQ(decoded.value().pixels.size(), pixels.size());
    AURORA_TEST_CHECK_EQ(decoded.value().pixels, pixels);
}

}  // namespace aurora::test_cases::utest_png
