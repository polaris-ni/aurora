/// 测试类型: unit
/// 目标单元: src/aurora/window/swizzle.h（swizzle_rgba_premul_to_bgra）
/// 测试说明: 覆盖 Wayland ARGB8888 上屏专用的「RGBA 直色 → BGRA 预乘 alpha」转换——
/// alpha=0（阴影外缘）RGB 归零、alpha=255（内容区）与旧 XRGB 直转逐值恒等、半透明按
/// c*a/255 预乘且字节序为小端 B,G,R,A。纯内存转换，无平台依赖。

#include <cstddef>
#include <cstdint>

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

}  // namespace aurora::test_cases::utest_window_swizzle
