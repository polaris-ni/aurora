/// 测试类型: unit
/// 目标单元: include/aurora/core/color_space.h
/// 测试说明: D0b 色彩管理分层策略——sRGB 传递曲线编解码边界、sRGB↔Display P3
/// 转换已知值（sRGB 红 → P3 ≈ (234,51,35)）、灰阶/白点恒等（D65 同白点、矩阵列和 1）、
/// alpha 保留、同空间原样返回、往返转换 ≤ ±1 LSB、越界分量夹取

#include <cstdint>

#include "aurora/core/color.h"
#include "aurora/core/color_space.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_color_space {

AURORA_TEST_CASE(srgb_transfer_curve_boundaries) {
    // 传递曲线端点：0↔0、255↔1（编码前夹取，负/超界输入安全）。
    AURORA_TEST_CHECK_EQ(srgb_transfer_decode(0), 0.0F);
    AURORA_TEST_CHECK_NEAR(srgb_transfer_decode(255), 1.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(srgb_transfer_encode(0.0F), 0);
    AURORA_TEST_CHECK_EQ(srgb_transfer_encode(1.0F), 255);
    AURORA_TEST_CHECK_EQ(srgb_transfer_encode(-0.5F), 0);   // 夹下界
    AURORA_TEST_CHECK_EQ(srgb_transfer_encode(1.5F), 255);  // 夹上界
    // 线性段拐点以下为线性（c/12.92）：8bit 1 ≈ 0.00392 → 线性段。
    AURORA_TEST_CHECK_NEAR(srgb_transfer_decode(1), 1.0F / 255.0F / 12.92F, 1e-7F);
}

AURORA_TEST_CASE(srgb_red_to_display_p3_known_value) {
    // sRGB 红 primaries 在 Display P3 下的标准值 ≈ (234, 51, 35)。
    const Color p3_red = srgb_to_display_p3(Color{255, 0, 0, 255});
    AURORA_TEST_CHECK_EQ(p3_red.r, 234);
    AURORA_TEST_CHECK_EQ(p3_red.g, 51);
    AURORA_TEST_CHECK_EQ(p3_red.b, 35);
    AURORA_TEST_CHECK_EQ(p3_red.a, 255);  // alpha 保留
}

AURORA_TEST_CASE(grayscale_and_white_are_invariant) {
    // D65 同白点 + 矩阵列和为 1 ⇒ 灰阶恒等（R=G=B）。
    for (const std::uint8_t v : {std::uint8_t{0}, std::uint8_t{64}, std::uint8_t{128}, std::uint8_t{255}}) {
        const Color gray{v, v, v, 200};
        const Color to_p3 = srgb_to_display_p3(gray);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(to_p3.r) - static_cast<int>(v)), 1);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(to_p3.g) - static_cast<int>(v)), 1);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(to_p3.b) - static_cast<int>(v)), 1);
        AURORA_TEST_CHECK_EQ(to_p3.a, 200);
    }
}

AURORA_TEST_CASE(same_space_conversion_is_identity) {
    const Color c{12, 34, 56, 78};
    AURORA_TEST_CHECK_TRUE(convert_color(c, ColorSpace::SRGB, ColorSpace::SRGB) == c);
    AURORA_TEST_CHECK_TRUE(convert_color(c, ColorSpace::DisplayP3, ColorSpace::DisplayP3) == c);
}

AURORA_TEST_CASE(roundtrip_within_tolerance) {
    // 验收锚点：8bit 往返（sRGB→P3→sRGB）逐通道 ≤ ±3 LSB。
    // 为何不是 ±1：矩阵行在饱和原色附近近似相消（如绿原色 r = 1.2249·Lr − 0.2249·Lg ≈ 0），
    // P3 侧 8bit 量化的半步经曲线斜率放大后落在相消行上，实测最大漂移 3 LSB（绿/青原色 r 通道）。
    // 覆盖：纯色三原色、混合色、色域边角、暗部/亮部台阶。
    const Color samples[] = {
        Color{255, 0, 0, 255}, Color{0, 255, 0, 255}, Color{0, 0, 255, 255}, Color{255, 255, 0, 255},
        Color{0, 255, 255, 255}, Color{255, 0, 255, 255}, Color{12, 34, 56, 255}, Color{200, 100, 50, 255},
        Color{1, 2, 3, 255}, Color{253, 254, 255, 255}, Color{90, 90, 200, 128},
    };
    for (const Color &c : samples) {
        const Color rt = display_p3_to_srgb(srgb_to_display_p3(c));
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt.r) - static_cast<int>(c.r)), 3);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt.g) - static_cast<int>(c.g)), 3);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt.b) - static_cast<int>(c.b)), 3);
        AURORA_TEST_CHECK_EQ(rt.a, c.a);
    }
}

AURORA_TEST_CASE(p3_first_roundtrip_within_one_lsb_for_in_gamut) {
    // 反向往返（P3→sRGB→P3）仅在**落在 sRGB 色域内**的 P3 颜色上保证 ±1 LSB——
    // 超出色域的 P3 颜色在 →sRGB 时被夹取（信息损失，宽色域 → 窄色域的固有约束），
    // 反向再转换会停在裁剪后的最近色（见 out_of_gamut_components_are_clamped）。
    const Color in_gamut[] = {
        Color{128, 128, 128, 255}, Color{12, 34, 56, 255}, Color{90, 90, 200, 255}, Color{200, 100, 50, 128},
    };
    for (const Color &c : in_gamut) {
        const Color rt2 = srgb_to_display_p3(display_p3_to_srgb(c));
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt2.r) - static_cast<int>(c.r)), 1);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt2.g) - static_cast<int>(c.g)), 1);
        AURORA_TEST_CHECK_LE(std::abs(static_cast<int>(rt2.b) - static_cast<int>(c.b)), 1);
        AURORA_TEST_CHECK_EQ(rt2.a, c.a);
    }
}

AURORA_TEST_CASE(out_of_gamut_components_are_clamped) {
    // P3 → sRGB：P3 纯红超出 sRGB 色域（线性分量 1.22 / -0.042 / -0.020），
    // 变换后逐分量夹到 [0,1] → 落在最近的 sRGB 可表示色（= sRGB 红）。输出必在 [0,255]。
    const Color srgb_red = display_p3_to_srgb(Color{255, 0, 0, 255});
    AURORA_TEST_CHECK_EQ(srgb_red.r, 255);
    AURORA_TEST_CHECK_EQ(srgb_red.g, 0);
    AURORA_TEST_CHECK_EQ(srgb_red.b, 0);
}

AURORA_TEST_CASE(golden_colorspace_convention_is_srgb) {
    // SSOT 约定锚点：golden 基准唯一色彩空间恒为 sRGB（逐位确定性红线，见 ROADMAP D1/G2）。
    // utest_offscreen 的 golden 注记（colorspace 字段）与本断言共同守卫该约定。
    AURORA_TEST_CHECK_TRUE(GOLDEN_COLORSPACE == ColorSpace::SRGB);
}

}  // namespace aurora::test_cases::utest_color_space
