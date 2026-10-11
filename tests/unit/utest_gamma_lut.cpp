/// 测试类型: unit
/// 目标单元: include/aurora/render/detail/gamma_lut.h (+ src/aurora/render/gamma_lut.cpp 的 init_gamma_tables)
/// 测试说明: 覆盖 sRGB <-> 线性光查找表门面：init_gamma_tables 幂等填充、srgb_to_linear /
/// linear_to_srgb（含越界 clamp）、linear_to_srgb_lut（同表直查）、blend_srgb_over（线性光空间
/// source-over 黄金混合，alpha=0/1 两极端与不透明中间值）。
/// 判据：端点到端正确（0<->0、255<->1）、clamp 把越界线性值夹到 [0,255]、blend 两端退化为 dst/src。

#include <cmath>

#include "aurora/render/detail/gamma_lut.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_gamma_lut {

AURORA_TEST_CASE(init_is_idempotent_and_fills_tables) {
    // 首次调用填充并置 ready；二次调用为幂等早退（不重复填、不崩溃）。
    aurora::detail::init_gamma_tables();
    const bool first_ready = aurora::detail::g_gamma_tables.ready.load();
    AURORA_TEST_CHECK(first_ready);
    aurora::detail::init_gamma_tables();
    AURORA_TEST_CHECK(aurora::detail::g_gamma_tables.ready.load());
    // 端点：sRGB 0 <-> 线性 0、sRGB 255 <-> 线性 1。
    AURORA_TEST_CHECK(std::abs(aurora::detail::srgb_to_linear(0) - 0.0F) < 1e-4F);
    AURORA_TEST_CHECK(std::abs(aurora::detail::srgb_to_linear(255) - 1.0F) < 1e-4F);
}

AURORA_TEST_CASE(linear_to_srgb_endpoints_and_clamp) {
    aurora::detail::init_gamma_tables();
    AURORA_TEST_CHECK_EQ(aurora::detail::linear_to_srgb(0.0F), static_cast<std::uint8_t>(0));
    AURORA_TEST_CHECK_EQ(aurora::detail::linear_to_srgb(1.0F), static_cast<std::uint8_t>(255));
    // 越界线性值被 clamp 到 [0,1]：负值落 0、超 1 落 255（与 0/1 端点一致）。
    AURORA_TEST_CHECK_EQ(aurora::detail::linear_to_srgb(-1.0F), static_cast<std::uint8_t>(0));
    AURORA_TEST_CHECK_EQ(aurora::detail::linear_to_srgb(2.0F), static_cast<std::uint8_t>(255));
}

AURORA_TEST_CASE(lut_index_variant_matches_table_lookup) {
    aurora::detail::init_gamma_tables();
    // 在合法 12 位量化区间取样，linear_to_srgb_lut 与 linear_to_srgb 必须逐位相同（同一张表）。
    for (int idx = 0; idx < aurora::detail::AURORA_LINEAR_TO_SRGB_SIZE; idx += 257) {
        AURORA_TEST_CHECK_EQ(
            aurora::detail::linear_to_srgb_lut(idx),
            aurora::detail::linear_to_srgb(static_cast<float>(idx) /
                                           static_cast<float>(aurora::detail::AURORA_LINEAR_TO_SRGB_SIZE - 1)));
    }
}

AURORA_TEST_CASE(blend_srgb_over_extremes_and_midpoint) {
    aurora::detail::init_gamma_tables();
    const std::uint8_t dst = 200;
    const std::uint8_t src = 40;
    // alpha = 1 完全覆盖 → src；alpha = 0 保留 → dst。
    AURORA_TEST_CHECK_EQ(aurora::detail::blend_srgb_over(dst, src, 1.0F), src);
    AURORA_TEST_CHECK_EQ(aurora::detail::blend_srgb_over(dst, src, 0.0F), dst);
    // alpha = 0.5：结果落在 dst 与 src 之间（线性光空间混合，靠近中段）。
    const std::uint8_t mid = aurora::detail::blend_srgb_over(dst, src, 0.5F);
    AURORA_TEST_CHECK(mid > src);
    AURORA_TEST_CHECK(mid < dst);
}

}  // namespace aurora::test_cases::utest_gamma_lut
