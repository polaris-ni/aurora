/// 测试类型: unit
/// 目标单元: include/aurora/core/math.h
/// 测试说明: 覆盖 saturate/saturate_u8 的区间钳制、截断语义、浮点极值行为、与手写 std::clamp 的逐位等价契约

#include <algorithm>
#include <cstdint>
#include <limits>

#include "aurora/core/math.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_math {

namespace au = aurora;

/// @brief saturate 把任意值夹到 [0, 1]，端点取值保持不变。
AURORA_TEST_CASE(saturate_clamps_into_unit_interval) {
    static_assert(au::saturate(0.5F) == 0.5F);
    static_assert(au::saturate(0.0F) == 0.0F);
    static_assert(au::saturate(1.0F) == 1.0F);
    static_assert(au::saturate(-0.25F) == 0.0F);
    static_assert(au::saturate(1.75F) == 1.0F);
    AURORA_TEST_CHECK_NEAR(au::saturate(0.5F), 0.5F, 1e-9F);
    AURORA_TEST_CHECK_EQ(au::saturate(-0.25F), 0.0F);
    AURORA_TEST_CHECK_EQ(au::saturate(1.75F), 1.0F);
}

/// @brief saturate_u8 先 clamp 到 [0, 255] 再截断转 uint8_t（非四舍五入）。
AURORA_TEST_CASE(saturate_u8_clamps_and_truncates) {
    static_assert(au::saturate_u8(128.0F) == 128);
    static_assert(au::saturate_u8(0.0F) == 0);
    static_assert(au::saturate_u8(255.0F) == 255);
    static_assert(au::saturate_u8(300.0F) == 255);
    static_assert(au::saturate_u8(-1.0F) == 0);
    static_assert(au::saturate_u8(254.9F) == 254);  // 截断而非舍入
    static_assert(au::saturate_u8(0.5F) == 0);
    AURORA_TEST_CHECK_EQ(au::saturate_u8(254.9F), 254);
    AURORA_TEST_CHECK_EQ(au::saturate_u8(300.0F), 255);
    AURORA_TEST_CHECK_EQ(au::saturate_u8(-1.0F), 0);
}

/// @brief 浮点极值：±inf 被夹到端点；NaN 因比较恒假而原样穿透（与 std::clamp 等价的设计结果）。
AURORA_TEST_CASE(saturate_handles_float_extremes) {
    constexpr auto inf = std::numeric_limits<float>::infinity();
    static_assert(au::saturate(inf) == 1.0F);
    static_assert(au::saturate(-inf) == 0.0F);
    static_assert(au::saturate_u8(inf) == 255);
    static_assert(au::saturate_u8(-inf) == 0);
    AURORA_TEST_CHECK_EQ(au::saturate(inf), 1.0F);
    AURORA_TEST_CHECK_EQ(au::saturate_u8(-inf), 0);

    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    AURORA_TEST_CHECK(std::isnan(au::saturate(nan)));  // NaN 穿透：两个比较均为 false，返回原值
}

/// @brief 与手写 std::clamp 的「逐位等价」契约在典型值网格上成立。
AURORA_TEST_CASE(matches_hand_written_clamp_semantics) {
    for (const float x : {-1.0F, 0.0F, 0.25F, 0.75F, 1.0F, 2.0F}) {
        AURORA_TEST_CHECK_EQ(au::saturate(x), std::clamp(x, 0.0F, 1.0F));
    }
    for (const float x : {-1.0F, 0.0F, 127.5F, 254.9F, 255.0F, 300.0F}) {
        AURORA_TEST_CHECK_EQ(static_cast<int>(au::saturate_u8(x)),
                             static_cast<int>(static_cast<std::uint8_t>(std::clamp(x, 0.0F, 255.0F))));
    }
}

}  // namespace aurora::test_cases::utest_math
