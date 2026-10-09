/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/csd_shadow_compose.h（compose_shadow_margins_bgra）
/// 测试说明: 覆盖软件 wl_shm 上屏路径的阴影 margin 合成器——四向外缘字透明归零、
/// 衰减环 alpha 由外向内递增（与 shadow_attenuation 同源公式）、角部两向欧氏距离组合、
/// 内容矩形（含边界行）一字不动、非黑基色按 (c*a+255)>>8 预乘且与 swizzle 算术逐值一致、
/// 重复合成功幂等、margin=0 整函数无操作；另覆盖 scale=2 的物理几何。纯内存，无平台依赖。

#include <cstdint>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/detail/csd_shadow_compose.h"  // 内部头：测试 TU 以 src/ 为附加包含根
#include "aurora/window/swizzle.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_csd_shadow_compose {

namespace {

constexpr std::uint32_t AURORA_SENTINEL_WORD = 0xDEADBEEFU;  ///< 缓冲槽残留哨兵（合成器应覆写 margin、保留内容）

/// @brief 标准夹具（scale=1）：内容 400×100 位于 (10,10)，缓冲 420×120，margin=10，blur=9。
struct Fixture {
    static constexpr int AURORA_MARGIN = csd::AURORA_SHADOW_MARGIN_DP;  // 10
    static constexpr int AURORA_CW = 400;
    static constexpr int AURORA_CH = 100;
    static constexpr int AURORA_W = AURORA_CW + (2 * AURORA_MARGIN);  // 420
    static constexpr int AURORA_H = AURORA_CH + (2 * AURORA_MARGIN);  // 120
    static constexpr float AURORA_BLUR =
        static_cast<float>(AURORA_MARGIN - csd::AURORA_SHADOW_BLUR_INSET_PX);  // 9
    std::vector<std::uint32_t> words;

    Fixture()
        : words(static_cast<std::size_t>(AURORA_W) * static_cast<std::size_t>(AURORA_H), AURORA_SENTINEL_WORD) {}

    /// @brief 以标准基色（黑 alpha=70）合成一次。
    auto compose(Color base = csd::AURORA_SHADOW_BASE_COLOR) -> void {
        csd::compose_shadow_margins_bgra(words.data(), AURORA_W, AURORA_H, AURORA_MARGIN, AURORA_MARGIN, AURORA_CW,
                                         AURORA_CH, AURORA_MARGIN, AURORA_BLUR, base);
    }

    [[nodiscard]] auto at(int x, int y) const -> std::uint32_t {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 测试夹具按下标取字
        return words[(static_cast<std::size_t>(y) * static_cast<std::size_t>(AURORA_W)) +
                     static_cast<std::size_t>(x)];
    }
};

/// @brief 基色黑、给定 alpha 时的预期 BGRA 预乘字（RGB 恒 0）。
[[nodiscard]] auto black_word(std::uint32_t a) -> std::uint32_t { return a << 24U; }

}  // namespace

AURORA_TEST_CASE(attenuation_factor_is_unity_inside_and_zeros_past_blur) {
    AURORA_TEST_CHECK_EQ(csd::shadow_attenuation(0.0F, 9.0F), 1.0F);
    AURORA_TEST_CHECK_EQ(csd::shadow_attenuation(-3.0F, 9.0F), 1.0F);
    AURORA_TEST_CHECK_EQ(csd::shadow_attenuation(9.0F, 9.0F), 0.0F);  // dist == blur 归零（无外缘硬切线）
    AURORA_TEST_CHECK_EQ(csd::shadow_attenuation(10.0F, 9.0F), 0.0F);
    AURORA_TEST_CHECK_GT(csd::shadow_attenuation(1.0F, 9.0F), 0.88F);  // f(1,9)=8/9
}

AURORA_TEST_CASE(outer_edge_and_far_corner_words_are_transparent) {
    Fixture f;
    f.compose();
    // 四向缓冲外缘（含四角）：dist >= blur，透明字 0（槽残留被清干净）。
    AURORA_TEST_CHECK_EQ(f.at(0, 60), 0U);
    AURORA_TEST_CHECK_EQ(f.at(Fixture::AURORA_W - 1, 60), 0U);
    AURORA_TEST_CHECK_EQ(f.at(210, 0), 0U);
    AURORA_TEST_CHECK_EQ(f.at(210, Fixture::AURORA_H - 1), 0U);
    AURORA_TEST_CHECK_EQ(f.at(0, 0), 0U);
    AURORA_TEST_CHECK_EQ(f.at(Fixture::AURORA_W - 1, Fixture::AURORA_H - 1), 0U);
    // 紧邻外缘的一列：左/上 dx=10=blur+1 也已归零。
    AURORA_TEST_CHECK_EQ(f.at(1, 60), 0U);
    AURORA_TEST_CHECK_EQ(f.at(210, 1), 0U);
}

AURORA_TEST_CASE(band_alpha_grades_inward_on_all_four_sides) {
    Fixture f;
    f.compose();
    // 左带：dx=5 → f=4/9 → a=31；dx=1（紧邻内容）→ f=8/9 → a=62。
    AURORA_TEST_CHECK_EQ(f.at(5, 60), black_word(31));
    AURORA_TEST_CHECK_EQ(f.at(Fixture::AURORA_MARGIN - 1, 60), black_word(62));
    // 右/下首条外延列同样 a=62（outside_dist 首列=1，四向对称）。
    AURORA_TEST_CHECK_EQ(f.at(Fixture::AURORA_MARGIN + Fixture::AURORA_CW, 60), black_word(62));
    AURORA_TEST_CHECK_EQ(f.at(210, Fixture::AURORA_MARGIN + Fixture::AURORA_CH), black_word(62));
    // 上/下中段带厚度同为 10：取 dy=5 点 a=31。
    AURORA_TEST_CHECK_EQ(f.at(210, 5), black_word(31));
    AURORA_TEST_CHECK_EQ(f.at(210, Fixture::AURORA_H - 1 - (Fixture::AURORA_MARGIN / 2)), black_word(31));
    // 严格向内单调（左带 dx 递减 → alpha 不减）。
    std::uint32_t prev = 0U;
    for (int x = 1; x < Fixture::AURORA_MARGIN; ++x) {
        const std::uint32_t a = f.at(x, 60) >> 24U;
        AURORA_TEST_CHECK_GE(a, prev);
        prev = a;
    }
}

AURORA_TEST_CASE(corner_pixels_combine_both_axis_distances) {
    Fixture f;
    f.compose();
    // 内角邻 (9,9)：dx=dy=1，dist=sqrt(2)≈1.414，f≈0.843，a≈59。
    AURORA_TEST_CHECK_EQ(f.at(9, 9) >> 24U, 59U);
    // 角部对角线上 alpha 比同列直边点衰减更快（距离两向组合）。
    AURORA_TEST_CHECK_LT(f.at(5, 5) >> 24U, f.at(5, 60) >> 24U);
}

AURORA_TEST_CASE(content_rect_words_are_never_touched) {
    Fixture f;
    f.compose();
    for (int y = Fixture::AURORA_MARGIN; y < Fixture::AURORA_MARGIN + Fixture::AURORA_CH; ++y) {
        for (int x = Fixture::AURORA_MARGIN; x < Fixture::AURORA_MARGIN + Fixture::AURORA_CW; ++x) {
            AURORA_TEST_CHECK_EQ(f.at(x, y), AURORA_SENTINEL_WORD);
        }
    }
}

AURORA_TEST_CASE(compose_is_idempotent_on_repeat) {
    Fixture f;
    f.compose();
    const std::vector<std::uint32_t> once = f.words;
    f.compose();
    AURORA_TEST_CHECK(f.words == once);
}

AURORA_TEST_CASE(nonzero_base_color_is_premultiplied_like_swizzle) {
    Fixture f;
    const Color base{10, 20, 30, 70};
    f.compose(base);
    // (9,60)：dx=1 → a=62；预期字与 swizzle_rgba_premul_to_bgra 的逐像素算术完全一致
    //（两路输出都进同一个 wl_shm 缓冲，算术漂移会在 margin/内容接缝处显形）。
    const std::uint32_t src_rgba = static_cast<std::uint32_t>(base.r) |
                                   (static_cast<std::uint32_t>(base.g) << 8U) |
                                   (static_cast<std::uint32_t>(base.b) << 16U) |
                                   (62U << 24U);  // 小端内存 R,G,B,A（Painter 像素序）
    std::uint32_t expected = 0U;
    swizzle_rgba_premul_to_bgra(&src_rgba, &expected, 1);
    AURORA_TEST_CHECK_EQ(f.at(9, 60), expected);
    AURORA_TEST_CHECK_NE(f.at(9, 60) & 0x00FFFFFFU, 0U);  // 非黑基色：预乘 RGB 通道非零
}

AURORA_TEST_CASE(zero_margin_is_a_noop) {
    std::vector<std::uint32_t> words(100U, AURORA_SENTINEL_WORD);
    csd::compose_shadow_margins_bgra(words.data(), 10, 10, 0, 0, 10, 10, 0, 9.0F, csd::AURORA_SHADOW_BASE_COLOR);
    for (const std::uint32_t word : words) {
        AURORA_TEST_CHECK_EQ(word, AURORA_SENTINEL_WORD);
    }
}

AURORA_TEST_CASE(scale_two_physical_geometry_grades_over_full_margin) {
    // scale=2：内容 800×200 位于 (20,20)，缓冲 840×240，margin=20px，blur=(10−1)*2=18px。
    constexpr int m = 20;
    constexpr int cw = 800;
    constexpr int ch = 200;
    constexpr int w = cw + (2 * m);
    constexpr int h = ch + (2 * m);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                     AURORA_SENTINEL_WORD);
    csd::compose_shadow_margins_bgra(words.data(), w, h, m, m, cw, ch, m, 18.0F, csd::AURORA_SHADOW_BASE_COLOR);
    auto at = [&](int x, int y) -> std::uint32_t {
        return words[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)];
    };
    AURORA_TEST_CHECK_EQ(at(0, 120), 0U);                       // 外缘归零（dx=20 > blur=18）
    AURORA_TEST_CHECK_EQ(at(10, 120) >> 24U, 31U);              // dx=10 → f=4/9 → a=31
    AURORA_TEST_CHECK_EQ(at(m - 1, 120) >> 24U, 66U);           // dx=1 → f=17/18 → a=66
    AURORA_TEST_CHECK_EQ(at(m, m), AURORA_SENTINEL_WORD);       // 内容角不触
}

}  // namespace aurora::test_cases::utest_csd_shadow_compose
