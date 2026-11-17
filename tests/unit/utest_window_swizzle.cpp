/// 测试类型: unit
/// 目标单元: src/aurora/window/swizzle.h（swizzle_rgba_premul_to_bgra / swizzle_rgba_to_bgra /
///            swizzle_bgra_to_rgba 与其标量参考、AVX2 内核）
/// 测试说明: 覆盖 Wayland ARGB8888 上屏专用的「RGBA 直色 → BGRA 预乘 alpha」转换——
/// alpha=0（阴影外缘）RGB 归零、alpha=255（内容区）与旧 XRGB 直转逐值恒等、半透明按
/// c*a/255 预乘且字节序为小端 B,G,R,A；另覆盖三处转换的「标量参考 / AVX2 内核 / 分发入口」
/// 三路逐字节一致（含整组边界与不足一组的尾部）。纯内存转换，无平台依赖。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "aurora/window/swizzle.h"  // 内部头：测试 TU 以 src/ 为附加包含根
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_swizzle {

namespace {

/// @brief 以 Painter 内存序（小端 R,G,B,A）打包一个 RGBA 像素字。
[[nodiscard]] auto pack_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) -> std::uint32_t {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8U) |
           (static_cast<std::uint32_t>(b) << 16U) | (static_cast<std::uint32_t>(a) << 24U);
}

struct BgraPixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
};

[[nodiscard]] auto decode_bgra(std::uint32_t word) -> BgraPixel {
    return BgraPixel{.b = static_cast<std::uint8_t>(word & 0xFFU),
                     .g = static_cast<std::uint8_t>((word >> 8U) & 0xFFU),
                     .r = static_cast<std::uint8_t>((word >> 16U) & 0xFFU),
                     .a = static_cast<std::uint8_t>((word >> 24U) & 0xFFU)};
}

/// @brief 生产实现同款整数近似：(c*a + 255) >> 8（误差 ≤ 1 LSB），测试逐字节断言用。
[[nodiscard]] auto premul(std::uint32_t c, std::uint32_t a) -> std::uint8_t {
    return static_cast<std::uint8_t>(((c * a) + 0xFFU) >> 8);
}

/// @brief 确定性伪随机像素填充（xorshift32），覆盖全 32 位字组合（含各档 alpha）。
void fill_random(std::vector<std::uint32_t> &buf, std::uint32_t seed) {
    std::uint32_t state = seed | 1U;
    for (std::uint32_t &word : buf) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        word = state;
    }
}

/// @brief 逐字比对两段等长像素，返回差异字数。
auto count_word_diffs(const std::vector<std::uint32_t> &lhs, const std::vector<std::uint32_t> &rhs) -> std::size_t {
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            ++diffs;
        }
    }
    return diffs;
}

/// @brief 覆盖整组边界与不足一组的尾部：1/7 为纯尾部，8 为恰好一组，9 与 1031 为「整组 + 尾部」。
constexpr std::size_t AURORA_SWIZZLE_PARITY_LENGTHS[] = {1, 7, 8, 9, 1031};

}  // namespace

AURORA_TEST_CASE(transparent_pixel_becomes_zero_rgb) {
    // alpha=0（阴影衰减外缘）：预乘后 BGR 全部归零、alpha 保留 0——半透黑叠加桌面时
    // 该像素完全透明，不会因为直色上传而泛灰。
    const std::uint32_t src[1] = {pack_rgba(120, 64, 200, 0)};
    std::uint32_t dst[1] = {0xDEADBEEFU};
    swizzle_rgba_premul_to_bgra(src, dst, 1);
    AURORA_TEST_CHECK_EQ(dst[0], 0U);
}

AURORA_TEST_CASE(opaque_pixel_matches_legacy_xrgb_swizzle) {
    // alpha=255（不透明内容区）：预乘是恒等变换，输出须与旧 swizzle_rgba_to_bgra 逐位相同，
    // 保证从 XRGB8888 切到 ARGB8888 后内容像素零差异。
    const std::uint32_t src[4] = {pack_rgba(10, 20, 30, 255), pack_rgba(0, 0, 0, 255), pack_rgba(255, 255, 255, 255),
                                  pack_rgba(7, 77, 177, 255)};
    std::uint32_t premul_dst[4] = {};
    std::uint32_t legacy_dst[4] = {};
    swizzle_rgba_premul_to_bgra(src, premul_dst, 4);
    swizzle_rgba_to_bgra(src, legacy_dst, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): 定长 4 元组逐字比对
        AURORA_TEST_CHECK_EQ(premul_dst[i], legacy_dst[i]);
    }
    const BgraPixel px = decode_bgra(premul_dst[0]);
    AURORA_TEST_CHECK_EQ(px.b, 30);  // R/B 交换
    AURORA_TEST_CHECK_EQ(px.g, 20);
    AURORA_TEST_CHECK_EQ(px.r, 10);
    AURORA_TEST_CHECK_EQ(px.a, 255);
}

AURORA_TEST_CASE(semi_transparent_pixel_is_premultiplied) {
    // 半透明阴影像素（近内容边 alpha=128）：每通道按 c*a/255 预乘，字节序 B,G,R,A。
    const std::uint32_t src[1] = {pack_rgba(10, 20, 30, 128)};
    std::uint32_t dst[1] = {};
    swizzle_rgba_premul_to_bgra(src, dst, 1);
    const BgraPixel px = decode_bgra(dst[0]);
    AURORA_TEST_CHECK_EQ(px.a, 128);
    AURORA_TEST_CHECK_EQ(px.r, premul(10, 128));
    AURORA_TEST_CHECK_EQ(px.g, premul(20, 128));
    AURORA_TEST_CHECK_EQ(px.b, premul(30, 128));
    // 预乘后各通道不超过 alpha（预乘格式的合法值域）。
    AURORA_TEST_CHECK(px.r <= px.a);
    AURORA_TEST_CHECK(px.g <= px.a);
    AURORA_TEST_CHECK(px.b <= px.a);
}

AURORA_TEST_CASE(avx2_kernels_match_scalar_byte_for_byte) {
    // 显式 AVX2 内核与标量参考逐字节一致：覆盖整组边界与尾部，预乘的整数乘/截断口径相同。
#ifdef AURORA_SWIZZLE_AVX2
    if (!aurora::detail::swizzle_avx2_available()) {
        AURORA_TEST_SKIP("host cpu has no avx2");
    }
    for (const std::size_t n : AURORA_SWIZZLE_PARITY_LENGTHS) {
        std::vector<std::uint32_t> src(n);
        std::vector<std::uint32_t> swap_ref(n);
        std::vector<std::uint32_t> swap_avx(n);
        std::vector<std::uint32_t> prem_ref(n);
        std::vector<std::uint32_t> prem_avx(n);
        fill_random(src, (static_cast<std::uint32_t>(n) * 2654435761U) + 7U);
        swizzle_rgba_to_bgra_scalar(src.data(), swap_ref.data(), n);
        aurora::detail::swap_rb_avx2(src.data(), swap_avx.data(), n);
        AURORA_TEST_CHECK_EQ(count_word_diffs(swap_ref, swap_avx), 0U);
        swizzle_rgba_premul_to_bgra_scalar(src.data(), prem_ref.data(), n);
        aurora::detail::premul_bgra_avx2(src.data(), prem_avx.data(), n);
        AURORA_TEST_CHECK_EQ(count_word_diffs(prem_ref, prem_avx), 0U);
    }
#else
    AURORA_TEST_SKIP("avx2 kernel is not compiled on this target");
#endif
}

AURORA_TEST_CASE(dispatch_entries_match_scalar_reference) {
    // 分发入口在任意运行环境下都必须与标量参考同结果：AVX2 可用时走内核，否则走参考。
    for (const std::size_t n : AURORA_SWIZZLE_PARITY_LENGTHS) {
        std::vector<std::uint32_t> src(n);
        std::vector<std::uint32_t> ref(n);
        std::vector<std::uint32_t> got(n);
        fill_random(src, (static_cast<std::uint32_t>(n) * 40503U) + 11U);
        swizzle_rgba_to_bgra_scalar(src.data(), ref.data(), n);
        swizzle_rgba_to_bgra(src.data(), got.data(), n);
        AURORA_TEST_CHECK_EQ(count_word_diffs(ref, got), 0U);
        swizzle_bgra_to_rgba(src.data(), got.data(), n);
        AURORA_TEST_CHECK_EQ(count_word_diffs(ref, got), 0U);
        swizzle_rgba_premul_to_bgra_scalar(src.data(), ref.data(), n);
        swizzle_rgba_premul_to_bgra(src.data(), got.data(), n);
        AURORA_TEST_CHECK_EQ(count_word_diffs(ref, got), 0U);
    }
}

}  // namespace aurora::test_cases::utest_window_swizzle
