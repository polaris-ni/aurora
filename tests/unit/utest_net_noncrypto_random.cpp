/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/noncrypto_random.h + src/aurora/net/detail/noncrypto_random.cpp
/// 测试说明: 库内置非密码学序列的取值域与推进性回归（`RetryPolicy::jitter` 的抖动位与 `X-Request-Id` 的
///           随机位共用这一份序列）。判据只有三条，且都是「形状」而非「具体数值」——序列以时钟播种，任何
///           逐值断言都只能在 `jitter = 0` 那条腿上做（那属 `utest_net_http_retry` 的口径）：
///           ① `noncrypto_unit()` 恒落 `[0, 1)` 且**上下两端都取得到**（`RetryPolicy::jitter` 的公共契约是
///           「等待在 `[算得值, 算得值 * (1 + jitter)]` 内取值」，若折算误除以 2^32，取值域被压成 `[0, 0.5)`，
///           契约只兑现一半——本用例的右端断言正是那条错误的猎枪）；② 逐次推进即逐次不同（进程内不重复是
///           链路标识的唯一硬要求）；③ `noncrypto_u64()` 取到的枚枚互异且位分布不偏（高 32 位拼接退化成一枚
///           常数时，`31 位 × 2` 的取值域会立刻在这里露出：连续多枚只会落在同一小段里）。

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "aurora/net/detail/noncrypto_random.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_noncrypto_random {
namespace {

using aurora::net::detail::noncrypto_u64;
using aurora::net::detail::noncrypto_unit;

/// @brief 取若干枚 `[0, 1)` 取值。
/// @param count 枚数。
/// @return 取值表。
[[nodiscard]] auto draw_units(std::size_t count) -> std::vector<double> {
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back(noncrypto_unit());
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(unit_values_spread_the_whole_half_open_interval) {
    const auto values = draw_units(2000);
    // 「铺满整个半开区间」按十等分桶命中判，不再拿「两两不重复」当代理：抽样撞一次生日碰撞
    // 就让互异计数少一，而序列是时钟播种的非密码学源、有限分辨率下碰撞是概率必然而非回归
    // （本用例曾以 1999/2000 偶发把 asan 腿染红）。覆盖判据对目标退化依旧敏感。
    constexpr int buckets = 10;
    std::vector<int> coverage(static_cast<std::size_t>(buckets), 0);
    double least = values.front();
    double most = values.front();
    for (const double value : values) {
        // 域本身：半开区间 `[0, 1)`，两端之外的值都不该出现。
        AURORA_TEST_CHECK(value >= 0.0 && value < 1.0);
        const int bucket = static_cast<int>(value * static_cast<double>(buckets));
        // 右端兜底，绝不越界：`value` 恰为 1.0 时 bucket 会顶到 10，夹回末桶。
        const auto index = static_cast<std::size_t>(std::clamp(bucket, 0, buckets - 1));
        ++coverage[index];
        least = std::min(least, value);
        most = std::max(most, value);
    }
    // 每桶都该有值：取值域若被压成 `[0, 0.5)`（除以 2^32 那一型错误），上半五桶全空，这里立刻露出来。
    for (const int hits : coverage) {
        AURORA_TEST_CHECK(hits > 0);
    }
    // 两端都要真的够得到：除以 2^32 的那一型错误只会让右端停在 0.5 以下。
    AURORA_TEST_CHECK(least < 0.05);
    AURORA_TEST_CHECK(most > 0.95);
}

AURORA_TEST_CASE(the_sequence_never_repeats_within_one_process) {
    std::set<std::uint64_t> seen;
    for (std::size_t index = 0; index < 1000; ++index) {
        AURORA_TEST_CHECK_EQ(seen.insert(noncrypto_u64()).second, true);
    }
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{1000});
}

AURORA_TEST_CASE(a_sixty_four_bit_draw_uses_both_halves) {
    // 拼接退化成一枚 31 位有效段时，取值会全部挤在低段：这里用「最高位与最低位都出现过 1」把两半都钉住。
    bool any_high_bit = false;
    bool any_low_bit = false;
    for (std::size_t index = 0; index < 200; ++index) {
        const std::uint64_t value = noncrypto_u64();
        any_high_bit = any_high_bit || (value & (UINT64_C(1) << 63)) != 0;
        any_low_bit = any_low_bit || (value & UINT64_C(1)) != 0;
    }
    AURORA_TEST_CHECK(any_high_bit);
    AURORA_TEST_CHECK(any_low_bit);
}

}  // namespace aurora::test_cases::utest_net_noncrypto_random