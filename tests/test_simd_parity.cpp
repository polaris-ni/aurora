// Aurora — SIMD 双实现逐位一致校验
//
// 比较同一 RGBA 缓冲区在 标量黄金 / SSE2 / AVX2 / 分发入口 四条路径下的逐字节结果。
// 仅比 RGB（alpha 恒被四条路径覆写为 255）。
//
// 关键约束：本 TU 必须以 -ffp-contract=off 编译（CMake 在 AURORA_ENABLE_SIMD=ON 时
// 已对 test_simd_parity 设置），否则标量参考会被编译器 FMA 融合，与显式 mul+add 的
// SIMD 路径产生差位，误报 parity 失败。
//
// 覆盖：alpha=0/255、非 8 倍数宽、随机源/目标、每通道 alpha（文本 AA 的 per-channel 覆盖率）、
//       linear 混合（fill_rect_slow_path 路径）。

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "aurora/render/detail/painter_simd.inl"
#include "test_harness.h"

namespace {

constexpr int AURORA_TEST_MAX_N = 64;

void fill_random(std::uint8_t *buf, const std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution dist(0, 255);
    for (std::size_t i = 0; i < n; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        buf[i] = static_cast<std::uint8_t>(dist(rng));
    }
}

auto rgb_equal(const std::uint8_t *a, const std::uint8_t *b, int n) -> bool {
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            if (a[(static_cast<std::size_t>(i) * 4U) + c] != b[(static_cast<std::size_t>(i) * 4U) + c]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

AURORA_TEST() {
    using aurora::detail::blend_linear_region;
    using aurora::detail::blend_linear_region_scalar;
    using aurora::detail::blend_srgb_over_region;
    using aurora::detail::blend_srgb_over_region_scalar;
    using aurora::detail::blur_region;
    using aurora::detail::blur_region_scalar;
    using aurora::detail::gradient_linear_fill;
    using aurora::detail::gradient_linear_scanline_scalar;
    using aurora::detail::gradient_radial_fill;
    using aurora::detail::gradient_radial_scanline_scalar;
    using aurora::detail::init_gamma_tables;

    // 四条路径共享同一 gamma LUT，必须先初始化，否则 LUT 为零 → 结果一致但无意义。
    init_gamma_tables();

    // 每通道 alpha 角用例：纯 0 / 纯 1 / 均匀半透 / 非对称 / 单通道覆盖（模拟文本 AA 的 fr/fg/fb）。
    const std::vector<std::array<float, 3>> alpha_cases = {
        {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.5F, 0.5F, 0.5F}, {0.25F, 0.75F, 0.1F},
        {0.0F, 0.5F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F},
    };
    // 覆盖非 4/8 倍数宽与边界。
    const std::vector widths = {0, 1, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64};

    int cases = 0;

    // ---------- 伽马混合逐位一致 ----------
    {
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_scalar{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_sse2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_avx2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_disp{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> ref{};
        for (std::uint32_t seed = 1; seed <= 200; ++seed) {
            fill_random(ref.data(), static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4, seed);
            const auto sr = static_cast<std::uint8_t>(seed & 0xFFU);
            const auto sg = static_cast<std::uint8_t>((seed * 7) & 0xFFU);
            const auto sb = static_cast<std::uint8_t>((seed * 13) & 0xFFU);
            for (const auto &a : alpha_cases) {
                for (int n : widths) {
                    if (n > AURORA_TEST_MAX_N) {
                        continue;
                    }
                    std::memcpy(buf_scalar.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_sse2.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_avx2.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_disp.data(), ref.data(), static_cast<std::size_t>(n) * 4U);

                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                    blend_srgb_over_region_scalar(buf_scalar.data(), sr, sg, sb, a[0], a[1], a[2], n);
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                    blend_srgb_over_region(buf_disp.data(), sr, sg, sb, a[0], a[1], a[2], n);
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                    aurora::detail::blend_srgb_over_region_sse2(buf_sse2.data(), sr, sg, sb, a[0], a[1], a[2], n);
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                    aurora::detail::blend_srgb_over_region_avx2(buf_avx2.data(), sr, sg, sb, a[0], a[1], a[2], n);
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_sse2.data(), n),
                                          "gamma: sse2 vs scalar mismatch");
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_avx2.data(), n),
                                          "gamma: avx2 vs scalar mismatch");
#endif
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_disp.data(), n),
                                          "gamma: dispatch vs scalar mismatch");
                    ++cases;
                }
            }
        }
    }

    // ---------- 线性混合逐位一致（fill_rect_slow_path 路径）----------
    {
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_scalar{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_sse2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_avx2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_disp{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> ref{};
        const std::vector alpha_f = {0.0F, 0.333F, 0.5F, 0.75F, 1.0F};
        for (std::uint32_t seed = 1; seed <= 200; ++seed) {
            fill_random(ref.data(), static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4, (seed * 31U) + 5U);
            const auto sr = static_cast<std::uint8_t>((seed * 3) & 0xFFU);
            const auto sg = static_cast<std::uint8_t>((seed * 11) & 0xFFU);
            const auto sb = static_cast<std::uint8_t>((seed * 17) & 0xFFU);
            for (float fa : alpha_f) {
                const float finv = 1.0F - fa;
                for (int n : widths) {
                    if (n > AURORA_TEST_MAX_N) {
                        continue;
                    }
                    std::memcpy(buf_scalar.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_sse2.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_avx2.data(), ref.data(), static_cast<std::size_t>(n) * 4U);
                    std::memcpy(buf_disp.data(), ref.data(), static_cast<std::size_t>(n) * 4U);

                    blend_linear_region_scalar(buf_scalar.data(), sr, sg, sb, fa, finv, n);
                    blend_linear_region(buf_disp.data(), sr, sg, sb, fa, finv, n);
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
                    aurora::detail::blend_linear_region_sse2(buf_sse2.data(), sr, sg, sb, fa, finv, n);
                    aurora::detail::blend_linear_region_avx2(buf_avx2.data(), sr, sg, sb, fa, finv, n);
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_sse2.data(), n),
                                          "linear: sse2 vs scalar mismatch");
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_avx2.data(), n),
                                          "linear: avx2 vs scalar mismatch");
#endif
                    AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_disp.data(), n),
                                          "linear: dispatch vs scalar mismatch");
                    ++cases;
                }
            }
        }
    }

    // ---------- 整数 box blur 逐位一致 ----------
    {
        const std::vector sizes = {1, 2, 4, 7, 8, 15, 16, 33, 64};
        const std::vector radii = {1, 2, 5, 10, 30};  // 含 r >> 尺寸，强压边缘 clamp
        std::vector<std::uint8_t> ref;
        std::vector<std::uint8_t> buf_scalar;
        std::vector<std::uint8_t> buf_sse2;
        std::vector<std::uint8_t> buf_avx2;
        std::vector<std::uint8_t> buf_disp;
        for (int rw : sizes) {
            for (int rh : sizes) {
                for (int r : radii) {
                    const std::size_t npix = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);
                    if (npix == 0) {
                        continue;
                    }
                    const std::size_t bytes = npix * 4U;
                    ref.assign(bytes, 0);
                    buf_scalar.assign(bytes, 0);
                    buf_sse2.assign(bytes, 0);
                    buf_avx2.assign(bytes, 0);
                    buf_disp.assign(bytes, 0);
                    std::mt19937 rng(static_cast<std::uint32_t>((rw * 131) + (rh * 17) + (r * 3) + 1));
                    std::uniform_int_distribution dist(0, 255);
                    for (std::size_t i = 0; i < bytes; ++i) {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                        ref[i] = static_cast<std::uint8_t>(dist(rng));
                    }
                    std::memcpy(buf_scalar.data(), ref.data(), bytes);
                    std::memcpy(buf_sse2.data(), ref.data(), bytes);
                    std::memcpy(buf_avx2.data(), ref.data(), bytes);
                    std::memcpy(buf_disp.data(), ref.data(), bytes);

                    blur_region_scalar(buf_scalar.data(), rw, 0, 0, rw, rh, r);
                    blur_region(buf_disp.data(), rw, 0, 0, rw, rh, r);
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
                    aurora::detail::blur_region_sse2(buf_sse2.data(), rw, 0, 0, rw, rh, r);
                    aurora::detail::blur_region_avx2(buf_avx2.data(), rw, 0, 0, rw, rh, r);
                    AURORA_TEST_CHECK_MSG(std::memcmp(buf_scalar.data(), buf_sse2.data(), bytes) == 0,
                                          "blur: sse2 vs scalar mismatch");
                    AURORA_TEST_CHECK_MSG(std::memcmp(buf_scalar.data(), buf_avx2.data(), bytes) == 0,
                                          "blur: avx2 vs scalar mismatch");
#endif
                    AURORA_TEST_CHECK_MSG(std::memcmp(buf_scalar.data(), buf_disp.data(), bytes) == 0,
                                          "blur: dispatch vs scalar mismatch");
                    ++cases;
                }
            }
        }
    }

    // ---------- 渐变扫描线逐位一致 ----------
    {
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_scalar{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_sse2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_avx2{};
        std::array<std::uint8_t, static_cast<std::size_t>(AURORA_TEST_MAX_N) * 4> buf_disp{};
        // 端点色（双色标、全不透明）：亮→暗、黑→白、红→绿、一般色、同色退化，覆盖不同通道差符号。
        const std::vector<std::array<std::uint8_t, 4>> color_pairs = {
            {250, 250, 255, 255}, {30, 30, 60, 255},  {0, 0, 0, 255},      {255, 255, 255, 255}, {255, 0, 0, 255},
            {0, 255, 0, 255},     {10, 200, 50, 255}, {230, 20, 180, 255}, {0, 0, 0, 255},       {0, 0, 0, 255},
        };
        const std::vector gwidths = {1, 3, 4, 5, 7, 8, 13, 15, 16, 17, 31, 32, 33, 63, 64};
        const std::vector stop0s = {0.0F, 0.25F, 0.5F};
        const std::vector ranges = {0.5F, 1.0F};
        // 逐位一致性校验要求结果可复现，固定种子为预期行为，非缺陷。
        // NOLINTNEXTLINE(bugprone-random-generator-seed)
        std::mt19937 rng(20240811U);
        std::uniform_real_distribution fr(-50.0F, 200.0F);
        std::uniform_real_distribution fpos(0.001F, 50.0F);

        // 线性渐变：扫描线逐位一致
        for (std::size_t cp = 0; cp + 1 < color_pairs.size(); cp += 2) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::uint8_t c0[4] = {color_pairs[cp][0], color_pairs[cp][1], color_pairs[cp][2], 255};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::uint8_t c1[4] = {color_pairs[cp + 1][0], color_pairs[cp + 1][1], color_pairs[cp + 1][2], 255};
            for (float stop0 : stop0s) {
                for (float range : ranges) {
                    for (int n : gwidths) {
                        if (n > AURORA_TEST_MAX_N) {
                            continue;
                        }
                        const float sx = fr(rng);
                        const float dx = fpos(rng);
                        const float sy = fr(rng);
                        const float dy = fpos(rng);
                        const float len_sq = (dx * dx) + (dy * dy);
                        const float inv_len_sq = len_sq > 0.001F ? 1.0F / len_sq : 0.0F;
                        for (int yk = 0; yk < 4; ++yk) {
                            const float py = (static_cast<float>(yk) - sy) * dy;
                            std::memset(buf_scalar.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_sse2.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_avx2.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_disp.data(), 0, static_cast<std::size_t>(n) * 4U);
                            gradient_linear_scanline_scalar(buf_scalar.data(), 0, n, sx, py, dx, dy, inv_len_sq, c0, c1,
                                                            stop0, range);
                            gradient_linear_fill(buf_disp.data(), 0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0,
                                                 range);
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
                            aurora::detail::gradient_linear_scanline_sse2(buf_sse2.data(), 0, n, sx, py, dx, dy,
                                                                          inv_len_sq, c0, c1, stop0, range);
                            aurora::detail::gradient_linear_scanline_avx2(buf_avx2.data(), 0, n, sx, py, dx, dy,
                                                                          inv_len_sq, c0, c1, stop0, range);
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_sse2.data(), n),
                                                  "grad lin: sse2 vs scalar mismatch");
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_avx2.data(), n),
                                                  "grad lin: avx2 vs scalar mismatch");
#endif
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_disp.data(), n),
                                                  "grad lin: dispatch vs scalar mismatch");
                            ++cases;
                        }
                    }
                }
            }
        }
        // 径向渐变：扫描线逐位一致（cy=0，扫多条 py 行）
        for (std::size_t cp = 0; cp + 1 < color_pairs.size(); cp += 2) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::uint8_t c0[4] = {color_pairs[cp][0], color_pairs[cp][1], color_pairs[cp][2], 255};
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::uint8_t c1[4] = {color_pairs[cp + 1][0], color_pairs[cp + 1][1], color_pairs[cp + 1][2], 255};
            for (float stop0 : stop0s) {
                for (float range : ranges) {
                    for (int n : gwidths) {
                        if (n > AURORA_TEST_MAX_N) {
                            continue;
                        }
                        const float cx = fr(rng);
                        const float inv_r = 1.0F / fpos(rng);
                        for (int yk = 0; yk < 4; ++yk) {
                            const float py = static_cast<float>(yk) * static_cast<float>(yk);  // (y-cy)^2, cy=0
                            std::memset(buf_scalar.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_sse2.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_avx2.data(), 0, static_cast<std::size_t>(n) * 4U);
                            std::memset(buf_disp.data(), 0, static_cast<std::size_t>(n) * 4U);
                            gradient_radial_scanline_scalar(buf_scalar.data(), 0, n, cx, py, inv_r, c0, c1, stop0,
                                                            range);
                            gradient_radial_fill(buf_disp.data(), 0, n, cx, py, inv_r, c0, c1, stop0, range);
#if defined(AURORA_ENABLE_SIMD) && defined(AURORA_SIMD_X86)
                            aurora::detail::gradient_radial_scanline_sse2(buf_sse2.data(), 0, n, cx, py, inv_r, c0, c1,
                                                                          stop0, range);
                            aurora::detail::gradient_radial_scanline_avx2(buf_avx2.data(), 0, n, cx, py, inv_r, c0, c1,
                                                                          stop0, range);
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_sse2.data(), n),
                                                  "grad rad: sse2 vs scalar mismatch");
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_avx2.data(), n),
                                                  "grad rad: avx2 vs scalar mismatch");
#endif
                            AURORA_TEST_CHECK_MSG(rgb_equal(buf_scalar.data(), buf_disp.data(), n),
                                                  "grad rad: dispatch vs scalar mismatch");
                            ++cases;
                        }
                    }
                }
            }
        }
    }

    AURORA_LOG_INFO("simd_parity", "total parity cases exercised:", std::to_string(cases).c_str());
    (void)cases;
}