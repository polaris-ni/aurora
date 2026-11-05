/// 测试类型: unit
/// 目标单元: include/aurora/render/detail/painter_simd.h
/// 测试说明: 光栅内核「标量黄金 ↔ SSE2 ↔ AVX2」五组三元组的逐字节一致性。混合两组覆盖非 4/8 倍数的
///           长度与多组参数（尾部回落标量恰是被向量化绕开的高发区），box blur 覆盖非对齐区域宽高与
///           两遍分离的窗口计数，渐变扫描线两组按各自返回值裁剪向量段、再按生产分发形态用标量补齐尾部
///           后比对整行。每组以同一份确定性伪随机输入分别喂三条路径，断言输出缓冲逐字节相等，并回报首个
///           差异的字节下标与两侧取值——恒等断言不算判据，能定位差异才算。
///           ⚠️ 平台与能力分支一律写在**用例体内**：`AURORA_ENABLE_SIMD` 关闭或非 x86 架构时 SIMD 函数
///           根本不存在，同名用例按既有纪律以 `AURORA_TEST_SKIP` 退让，声明本身不被 `#ifdef` 摘掉，
///           以免 `registry_integrity` 在另一种构建里单边失踪。含 `painter_simd.inl` 是为拿到与
///           `painter.cpp` 同源的内联定义，不复刻一份实现去比对复刻件。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/render/detail/gamma_lut.h"
#include "aurora/render/detail/painter_simd.h"
#include "aurora/render/detail/painter_simd.inl"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_simd_parity {
namespace {

using Bytes = std::vector<std::uint8_t>;
using aurora::detail::init_gamma_tables;

/// @brief 每像素字节数（RGBA）。
constexpr std::size_t AURORA_BYTES_PER_PIXEL = 4U;

/// @brief SSE2 一次处理的像素数。
constexpr int AURORA_SSE2_LANES = 4;

/// @brief AVX2 一次处理的像素数。
constexpr int AURORA_AVX2_LANES = 8;

/// @brief 造一份确定性伪随机像素缓冲：同一 seed 必得同一份输入，不读时钟、不碰文件系统。
/// @param pixels 像素个数。
/// @param seed 线性同余初值。
/// @return 长度为 pixels*4 的字节缓冲。
auto patterned(int pixels, std::uint32_t seed) -> Bytes {
    Bytes out(static_cast<std::size_t>(pixels) * AURORA_BYTES_PER_PIXEL);
    std::uint32_t state = seed;
    for (auto &byte : out) {
        state = (state * 1664525U) + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }
    return out;
}

/// @brief 首个差异的诊断文本；相等时给出「一致 + 字节数」，便于确认比对区间非空。
/// @param label 组别标签。
/// @param golden 标量黄金输出。
/// @param actual 待核对的 SIMD 输出。
/// @return 诊断文本。
auto difference(std::string_view label, const Bytes &golden, const Bytes &actual) -> std::string {
    const auto common = golden.size() < actual.size() ? golden.size() : actual.size();
    for (std::size_t i = 0; i < common; ++i) {
        if (golden[i] != actual[i]) {
            return std::string{label} + ": byte " + std::to_string(i) + " golden=" + std::to_string(golden[i]) +
                   " actual=" + std::to_string(actual[i]);
        }
    }
    if (golden.size() != actual.size()) {
        return std::string{label} + ": size golden=" + std::to_string(golden.size()) +
               " actual=" + std::to_string(actual.size());
    }
    return std::string{label} + ": identical over " + std::to_string(common) + " bytes";
}

/// @brief 逐字节比对一条 SIMD 输出；非空缓冲是判据前提，空缓冲会让「相等」变成恒真断言。
/// @param label 组别标签（进失败诊断）。
/// @param golden 标量黄金输出。
/// @param actual 待核对的 SIMD 输出。
auto expect_identical(std::string_view label, const Bytes &golden, const Bytes &actual) -> void {
    AURORA_TEST_REQUIRE_MSG(!golden.empty(), std::string{label} + ": golden buffer is empty");
    AURORA_TEST_CHECK_MSG(golden == actual, difference(label, golden, actual));
}

/// @brief 没有 SIMD 档位或不是 x86 时各用例的统一退让理由（`skip_case` 收 `std::string`，故留 C 串形态）。
constexpr auto AURORA_NO_SIMD_REASON =
    "this build has no SSE2/AVX2 kernels (AURORA_ENABLE_SIMD off or non-x86 target), parity has nothing to compare";

#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)

/// @brief 五组共用的长度集合：全部不是 8 的倍数，混合与扫描线各有一处尾部回落。
constexpr std::array<int, 5> AURORA_LENGTHS{1, 5, 7, 13, 37};

/// @brief 伽马混合组：三条实现写同一份输入，逐字节比对。
auto check_srgb_blend() -> void {
    const std::array<std::array<float, 6>, 3> cases{{
        {11.0F, 201.0F, 77.0F, 0.0F, 0.5F, 1.0F},
        {233.0F, 19.0F, 128.0F, 1.0F, 0.25F, 0.0F},
        {90.0F, 90.0F, 200.0F, 0.333F, 0.667F, 0.875F},
    }};
    for (const auto &params : cases) {
        const auto sr = static_cast<std::uint8_t>(params[0]);
        const auto sg = static_cast<std::uint8_t>(params[1]);
        const auto sb = static_cast<std::uint8_t>(params[2]);
        for (const int n : AURORA_LENGTHS) {
            auto golden = patterned(n, 0x51A7U + static_cast<std::uint32_t>(n));
            auto sse2 = golden;
            auto avx2 = golden;
            aurora::detail::blend_srgb_over_region_scalar(golden.data(), sr, sg, sb, params[3], params[4], params[5],
                                                          n);
            aurora::detail::blend_srgb_over_region_sse2(sse2.data(), sr, sg, sb, params[3], params[4], params[5], n);
            aurora::detail::blend_srgb_over_region_avx2(avx2.data(), sr, sg, sb, params[3], params[4], params[5], n);
            const std::string label = "srgb n=" + std::to_string(n) + " sr=" + std::to_string(sr);
            expect_identical(label + " sse2", golden, sse2);
            expect_identical(label + " avx2", golden, avx2);
        }
    }
}

/// @brief 线性混合组：三条实现写同一份输入，逐字节比对。
auto check_linear_blend() -> void {
    const std::array<std::array<float, 5>, 3> cases{{
        {17.0F, 199.0F, 63.0F, 0.0F, 1.0F},
        {240.0F, 31.0F, 111.0F, 1.0F, 0.0F},
        {88.0F, 144.0F, 210.0F, 0.35F, 0.65F},
    }};
    for (const auto &params : cases) {
        const auto sr = static_cast<std::uint8_t>(params[0]);
        const auto sg = static_cast<std::uint8_t>(params[1]);
        const auto sb = static_cast<std::uint8_t>(params[2]);
        for (const int n : AURORA_LENGTHS) {
            auto golden = patterned(n, 0x71B3U + static_cast<std::uint32_t>(n));
            auto sse2 = golden;
            auto avx2 = golden;
            aurora::detail::blend_linear_region_scalar(golden.data(), sr, sg, sb, params[3], params[4], n);
            aurora::detail::blend_linear_region_sse2(sse2.data(), sr, sg, sb, params[3], params[4], n);
            aurora::detail::blend_linear_region_avx2(avx2.data(), sr, sg, sb, params[3], params[4], n);
            const std::string label = "linear n=" + std::to_string(n) + " sr=" + std::to_string(sr);
            expect_identical(label + " sse2", golden, sse2);
            expect_identical(label + " avx2", golden, avx2);
        }
    }
}

/// @brief box blur 组：非对齐区域宽高 + 半径，比较整块帧缓冲。
auto check_box_blur() -> void {
    constexpr int full_width = 19;
    constexpr int full_height = 13;
    const std::array<std::array<int, 5>, 3> regions{{
        {2, 1, 13, 9, 1},
        {0, 0, 11, 7, 2},
        {3, 2, 7, 5, 3},
    }};
    for (const auto &region : regions) {
        const auto seed = patterned(full_width * full_height, 0x91C5U + static_cast<std::uint32_t>(region[2]));
        auto golden = seed;
        auto sse2 = seed;
        auto avx2 = seed;
        aurora::detail::blur_region_scalar(golden.data(), full_width, region[0], region[1], region[2], region[3],
                                           region[4]);
        aurora::detail::blur_region_sse2(sse2.data(), full_width, region[0], region[1], region[2], region[3],
                                         region[4]);
        aurora::detail::blur_region_avx2(avx2.data(), full_width, region[0], region[1], region[2], region[3],
                                         region[4]);
        const std::string label = "blur rw=" + std::to_string(region[2]) + " r=" + std::to_string(region[4]);
        expect_identical(label + " sse2", golden, sse2);
        expect_identical(label + " avx2", golden, avx2);
    }
}

/// @brief 扫描线公共参数：端点色与几何取非平凡值，避免整行落在钳位端点上成为恒真比对。
struct GradientParams {
    float sx = 3.25F;
    float py = 2.25F;
    float dx = 0.75F;
    float dy = 0.0F;
    float inv_len_sq = 1.0F / 12.5F;
    float stop0 = 0.15F;
    float range = 0.7F;
    float cx = 6.5F;
    float inv_r = 1.0F / 9.0F;
    std::array<std::uint8_t, 3> c0{17U, 91U, 203U};
    std::array<std::uint8_t, 3> c1{221U, 43U, 7U};
};

/// @brief 线性渐变扫描线组：SIMD 填向量段，尾部按生产分发形态用标量补齐后比整行。
auto check_linear_gradient() -> void {
    const GradientParams params;
    for (const int n : AURORA_LENGTHS) {
        auto golden = patterned(n, 0xB1D7U + static_cast<std::uint32_t>(n));
        auto sse2 = golden;
        auto avx2 = golden;
        const auto scalar = [&](Bytes &buffer) -> void {
            aurora::detail::gradient_linear_scanline_scalar(buffer.data(), 0, n, params.sx, params.py, params.dx,
                                                            params.dy, params.inv_len_sq, params.c0.data(),
                                                            params.c1.data(), params.stop0, params.range);
        };
        scalar(golden);
        const int sse2_done = aurora::detail::gradient_linear_scanline_sse2(
            sse2.data(), 0, n, params.sx, params.py, params.dx, params.dy, params.inv_len_sq, params.c0.data(),
            params.c1.data(), params.stop0, params.range);
        const int avx2_done = aurora::detail::gradient_linear_scanline_avx2(
            avx2.data(), 0, n, params.sx, params.py, params.dx, params.dy, params.inv_len_sq, params.c0.data(),
            params.c1.data(), params.stop0, params.range);
        AURORA_TEST_CHECK_EQ(sse2_done % AURORA_SSE2_LANES, 0);
        AURORA_TEST_CHECK_EQ(avx2_done % AURORA_AVX2_LANES, 0);
        AURORA_TEST_CHECK_MSG(sse2_done >= 0 && sse2_done <= n, "sse2 filled count out of range");
        AURORA_TEST_CHECK_MSG(avx2_done >= 0 && avx2_done <= n, "avx2 filled count out of range");
        // 生产分发在 SIMD 返回值之后以标量补齐尾部；同形补齐后整行才可与标量黄金逐字节对齐。
        aurora::detail::gradient_linear_scanline_scalar(
            &sse2[sse2_done * AURORA_BYTES_PER_PIXEL], sse2_done, n - sse2_done, params.sx, params.py, params.dx,
            params.dy, params.inv_len_sq, params.c0.data(), params.c1.data(), params.stop0, params.range);
        aurora::detail::gradient_linear_scanline_scalar(
            &avx2[avx2_done * AURORA_BYTES_PER_PIXEL], avx2_done, n - avx2_done, params.sx, params.py, params.dx,
            params.dy, params.inv_len_sq, params.c0.data(), params.c1.data(), params.stop0, params.range);
        expect_identical("linear-gradient n=" + std::to_string(n) + " sse2", golden, sse2);
        expect_identical("linear-gradient n=" + std::to_string(n) + " avx2", golden, avx2);
    }
}

/// @brief 径向渐变扫描线组：同上，另覆盖 sqrt 与半径归一的浮点序列。
auto check_radial_gradient() -> void {
    const GradientParams params;
    for (const int n : AURORA_LENGTHS) {
        auto golden = patterned(n, 0xD1E9U + static_cast<std::uint32_t>(n));
        auto sse2 = golden;
        auto avx2 = golden;
        aurora::detail::gradient_radial_scanline_scalar(golden.data(), 0, n, params.cx, params.py, params.inv_r,
                                                        params.c0.data(), params.c1.data(), params.stop0, params.range);
        const int sse2_done = aurora::detail::gradient_radial_scanline_sse2(
            sse2.data(), 0, n, params.cx, params.py, params.inv_r, params.c0.data(), params.c1.data(), params.stop0,
            params.range);
        const int avx2_done = aurora::detail::gradient_radial_scanline_avx2(
            avx2.data(), 0, n, params.cx, params.py, params.inv_r, params.c0.data(), params.c1.data(), params.stop0,
            params.range);
        AURORA_TEST_CHECK_EQ(sse2_done % AURORA_SSE2_LANES, 0);
        AURORA_TEST_CHECK_EQ(avx2_done % AURORA_AVX2_LANES, 0);
        AURORA_TEST_CHECK_MSG(sse2_done >= 0 && sse2_done <= n, "radial sse2 filled count out of range");
        AURORA_TEST_CHECK_MSG(avx2_done >= 0 && avx2_done <= n, "radial avx2 filled count out of range");
        aurora::detail::gradient_radial_scanline_scalar(&sse2[sse2_done * AURORA_BYTES_PER_PIXEL], sse2_done,
                                                        n - sse2_done, params.cx, params.py, params.inv_r,
                                                        params.c0.data(), params.c1.data(), params.stop0, params.range);
        aurora::detail::gradient_radial_scanline_scalar(&avx2[avx2_done * AURORA_BYTES_PER_PIXEL], avx2_done,
                                                        n - avx2_done, params.cx, params.py, params.inv_r,
                                                        params.c0.data(), params.c1.data(), params.stop0, params.range);
        expect_identical("radial-gradient n=" + std::to_string(n) + " sse2", golden, sse2);
        expect_identical("radial-gradient n=" + std::to_string(n) + " avx2", golden, avx2);
    }
}

#endif

}  // namespace

AURORA_TEST_CASE(srgb_blend_region_is_bitwise_identical_across_kernels) {
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
    init_gamma_tables();
    check_srgb_blend();
#else
    AURORA_TEST_SKIP(AURORA_NO_SIMD_REASON);
#endif
}

AURORA_TEST_CASE(linear_blend_region_is_bitwise_identical_across_kernels) {
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
    check_linear_blend();
#else
    AURORA_TEST_SKIP(AURORA_NO_SIMD_REASON);
#endif
}

AURORA_TEST_CASE(box_blur_region_is_bitwise_identical_across_kernels) {
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
    check_box_blur();
#else
    AURORA_TEST_SKIP(AURORA_NO_SIMD_REASON);
#endif
}

AURORA_TEST_CASE(linear_gradient_scanline_is_bitwise_identical_across_kernels) {
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
    check_linear_gradient();
#else
    AURORA_TEST_SKIP(AURORA_NO_SIMD_REASON);
#endif
}

AURORA_TEST_CASE(radial_gradient_scanline_is_bitwise_identical_across_kernels) {
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
    check_radial_gradient();
#else
    AURORA_TEST_SKIP(AURORA_NO_SIMD_REASON);
#endif
}

}  // namespace aurora::test_cases::utest_simd_parity
