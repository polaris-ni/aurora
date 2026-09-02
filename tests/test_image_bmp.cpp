// test_image_bmp.cpp — 内置 24 位 BMP 解码器（`detail::load_bmp`）1:1 测试，
// 重点覆盖安全审计 2026-08 修复的三类攻击者可控输入：
//   A) 像素区截断：仅 54 字节头却声明 1000×1000 → 修复前从堆上越界读 ~3MB 并返回给调用方
//      （堆信息泄露）；现须以 "io-parse-failed"（BMP pixel data truncated）拒绝。
//   B) 超大维度：raw_w = 0x7FFFFFFF 使 `w * 3` 有符号溢出（UB），row_bytes 变负、
//      转 size_t 后成为天文偏移量；现须被 k_bmp_max_dimension(65535) 上限拒绝。
//   C) 非 24 位 / 压缩变体：实际行距与解码器假设的 w*3 不同，放行即按错误行距整幅越界读。
// 全部用内存字节字面量构造，不落盘、不依赖任何资源路径。
#include <cstdint>
#include <vector>

#include "aurora/core/image.h"

#include "test_harness.h"

namespace detail = aurora::detail;
using aurora::Image;

namespace {

/// @brief 写小端 32 位到 buf[off..off+3]。
auto put32(std::vector<std::uint8_t> &b, std::size_t off, std::uint32_t v) -> void {
    b[off + 0] = static_cast<std::uint8_t>(v & 0xFFU);
    b[off + 1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    b[off + 2] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    b[off + 3] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
}

/// @brief 写小端 16 位到 buf[off..off+1]。
auto put16(std::vector<std::uint8_t> &b, std::size_t off, std::uint16_t v) -> void {
    b[off + 0] = static_cast<std::uint8_t>(v & 0xFFU);
    b[off + 1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU); // NOLINT(*-signed-bitwise)
}

/// @brief 构造一个 BITMAPINFOHEADER 形态的 BMP 头（54 字节），可自由指定各字段。
/// @param w 位图宽度（像素）。
/// @param h 位图高度（像素）。
/// @param bit_count 每像素位数（默认 24）。
/// @param compression 压缩方式（默认 BI_RGB=0）。
/// @param data_off 像素数据起始偏移（默认 54）。
/// @param include_pixels 为真时按 w/h 追加正确大小的像素区（全 0），否则只留 54 字节头。
auto make_bmp(std::uint32_t w, std::uint32_t h, std::uint16_t bit_count = 24, std::uint32_t compression = 0,
              std::uint32_t data_off = 54, bool include_pixels = false) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> b(54, 0);
    b[0] = 'B';
    b[1] = 'M';
    put32(b, 10, data_off);    // bfOffBits
    put32(b, 14, 40);          // biSize (BITMAPINFOHEADER)
    put32(b, 18, w);           // biWidth
    put32(b, 22, h);           // biHeight
    put16(b, 26, 1);           // biPlanes
    put16(b, 28, bit_count);   // biBitCount
    put32(b, 30, compression); // biCompression
    if (include_pixels) {
        const std::size_t row_bytes = (((static_cast<std::size_t>(w) * 3u) + 3u) / 4u) * 4u;
        b.resize(static_cast<std::size_t>(data_off) + (row_bytes * static_cast<std::size_t>(h)), 0);
    }
    put32(b, 2, static_cast<std::uint32_t>(b.size())); // bfSize
    return b;
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_image_bmp ===\n");

    // ---- 0) 正常路径：2×2 24 位 BMP 往返，验证 BGR→RGBA 与自底向上翻转 ----
    {
        // 2 像素/行 × 3 字节 = 6，按 4 字节对齐 → row_bytes = 8。
        auto b = make_bmp(2, 2, 24, 0, 54, /*include_pixels=*/true);
        constexpr std::size_t off = 54;
        constexpr std::size_t row = 8;
        // 文件第 0 行（= 图像最后一行）：像素 (0,1)=蓝, (1,1)=绿（BMP 存 BGR）。
        b[off + (0 * row) + 0] = 255;
        b[off + (0 * row) + 1] = 0;
        b[off + (0 * row) + 2] = 0; // B
        b[off + (0 * row) + 3] = 0;
        b[off + (0 * row) + 4] = 255;
        b[off + (0 * row) + 5] = 0; // G
        // 文件第 1 行（= 图像第 0 行）：像素 (0,0)=红, (1,0)=白。
        b[off + (1 * row) + 0] = 0;
        b[off + (1 * row) + 1] = 0;
        b[off + (1 * row) + 2] = 255; // R
        b[off + (1 * row) + 3] = 255;
        b[off + (1 * row) + 4] = 255;
        b[off + (1 * row) + 5] = 255; // 白

        auto r = detail::load_bmp(b);
        AURORA_TEST_CHECK_MSG(r.ok(), "valid 2x2 24-bit BMP decoded successfully");
        const Image &img = r.value();
        AURORA_TEST_CHECK_EQ(img.width, 2);
        AURORA_TEST_CHECK_EQ(img.height, 2);
        AURORA_TEST_CHECK_MSG(img.pixels.size() == static_cast<std::size_t>(img.width) * img.height * 4u,
                              "pixel buffer length == w*h*4 (Image invariant)");
        // 图像第 0 行第 0 列 = 红（来自文件最后一行）→ 证明自底向上翻转正确。
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[0]), 255); // R
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[1]), 0);   // G
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[2]), 0);   // B
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[3]), 255); // A
        // 图像最后一行第 0 列 = 蓝 → BGR→RGBA 通道序正确。
        constexpr std::size_t last = static_cast<std::size_t>((1 * 2) + 0) * 4u;
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[last + 0]), 0);   // R
        AURORA_TEST_CHECK_EQ(static_cast<int>(img.pixels[last + 2]), 255); // B
    }

    // ---- A) 像素区截断：只有 54 字节头，却声明 1000×1000 ----
    // 修复前：解码器仅校验 data_off 落在缓冲内，随后按 row_bytes*h 读取 → 越界读 ~3MB 堆内存。
    {
        // A1) data_off(54) == size(54)：先被「data_off 必须落在缓冲内」拦下。
        auto b = make_bmp(1000, 1000, 24, 0, 54, /*include_pixels=*/false);
        AURORA_TEST_CHECK_EQ(b.size(), static_cast<std::size_t>(54));
        auto r = detail::load_bmp(b);
        AURORA_TEST_CHECK_MSG(!r.ok(), "bare 54-byte header declaring 1000x1000 rejected (no out-of-bounds heap read)");
        AURORA_TEST_CHECK_MSG(r.error().code == "layout-invalid-constraints",
                              "data_off == size takes offset-validation branch");

        // A2) data_off 严格落在缓冲内（多 1 字节像素区），越过偏移校验后必须被截断校验拦下。
        // 这才是「修复前会越界读 ~3MB」的那条路径。
        b.push_back(0);
        auto r2 = detail::load_bmp(b);
        AURORA_TEST_CHECK_MSG(!r2.ok(),
                              "only 1 byte of pixel data but declares 1000x1000, rejected (truncation check active)");
        AURORA_TEST_CHECK_MSG(r2.error().code == "io-parse-failed",
                              "truncated pixel region error code is io-parse-failed");
    }

    // ---- A2) 像素区仅差 1 字节（off-by-one 边界）----
    {
        auto b = make_bmp(4, 4, 24, 0, 54, /*include_pixels=*/true);
        b.pop_back(); // 少一个字节
        auto r = detail::load_bmp(b);
        AURORA_TEST_CHECK_MSG(!r.ok(), "pixel region short by 1 byte is rejected (need > available bytes)");
        AURORA_TEST_CHECK_MSG(r.error().code == "io-parse-failed",
                              "off-by-one truncation error code is io-parse-failed");
    }

    // ---- B) 超大维度：0x7FFFFFFF 会让 w*3 有符号溢出（UB）----
    {
        auto r = detail::load_bmp(make_bmp(0x7FFFFFFFu, 1));
        AURORA_TEST_CHECK_MSG(!r.ok(), "width 0x7FFFFFFF rejected by upper bound (avoids w*3 signed overflow)");
        AURORA_TEST_CHECK_MSG(r.error().code == "layout-invalid-constraints",
                              "oversized width error code is layout-invalid-constraints");

        auto r2 = detail::load_bmp(make_bmp(1, 0x7FFFFFFFu));
        AURORA_TEST_CHECK_MSG(!r2.ok(), "height 0x7FFFFFFF rejected by upper bound");

        // 恰好越过 65535 上限（65536）须拒绝，65535 本身仅因像素区不足而被截断校验拒绝。
        auto r3 = detail::load_bmp(make_bmp(65536, 1));
        AURORA_TEST_CHECK_MSG(!r3.ok(), "width 65536 exceeds k_bmp_max_dimension upper bound");
        AURORA_TEST_CHECK_MSG(r3.error().code == "layout-invalid-constraints",
                              "65536 takes dimension upper-bound branch");
    }

    // ---- B2) 零维度 ----
    {
        AURORA_TEST_CHECK_MSG(!detail::load_bmp(make_bmp(0, 4)).ok(), "width 0 rejected");
        AURORA_TEST_CHECK_MSG(!detail::load_bmp(make_bmp(4, 0)).ok(), "height 0 rejected");
    }

    // ---- B3) data_off 越界 / 指向缓冲尾部 ----
    {
        auto b = make_bmp(2, 2, 24, 0, /*data_off=*/0xFFFFFFFFu, /*include_pixels=*/false);
        auto r = detail::load_bmp(b);
        AURORA_TEST_CHECK_MSG(!r.ok(), "data_off beyond buffer rejected");
        AURORA_TEST_CHECK_MSG(r.error().code == "layout-invalid-constraints",
                              "out-of-bounds data_off takes dimension/offset validation branch");
    }

    // ---- C) 非 24 位 / 压缩变体：行距假设不成立，必须拒绝而非按 w*3 越界读 ----
    {
        for (std::uint16_t bpp :
             { std::uint16_t{ 1 }, std::uint16_t{ 4 }, std::uint16_t{ 8 }, std::uint16_t{ 16 }, std::uint16_t{ 32 } }) {
            auto b = make_bmp(4, 4, bpp, 0, 54, /*include_pixels=*/true);
            auto r = detail::load_bmp(b);
            AURORA_TEST_CHECK_MSG(!r.ok(), "non-24-bit BMP rejected (row stride != w*3)");
            AURORA_TEST_CHECK_MSG(r.error().code == "io-parse-failed", "non-24-bit error code is io-parse-failed");
        }
        // RLE8 / RLE4 / BITFIELDS 压缩
        for (std::uint32_t comp : { 1u, 2u, 3u }) {
            auto b = make_bmp(4, 4, 24, comp, 54, /*include_pixels=*/true);
            AURORA_TEST_CHECK_MSG(!detail::load_bmp(b).ok(), "compressed BMP rejected");
        }
    }

    // ---- D) 头部本身过短 / 非 BMP 魔数 ----
    {
        AURORA_TEST_CHECK_MSG(!detail::load_bmp(std::vector<std::uint8_t>{}).ok(), "empty buffer rejected");
        AURORA_TEST_CHECK_MSG(!detail::load_bmp(std::vector<std::uint8_t>(53, 0)).ok(), "53 bytes (< 54) rejected");
        std::vector<std::uint8_t> not_bmp(64, 0);
        not_bmp[0] = 'P';
        not_bmp[1] = 'K';
        auto r = detail::load_bmp(not_bmp);
        AURORA_TEST_CHECK_MSG(!r.ok(), "non-BM magic rejected");
        AURORA_TEST_CHECK_MSG(r.error().code == "io-parse-failed", "wrong magic error code is io-parse-failed");
    }
}
