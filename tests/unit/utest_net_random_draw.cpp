/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/random_draw.h + src/aurora/net/detail/random_draw.cpp
/// 测试说明: 系统熵取数封装的映射与推进回归（`RetryPolicy::jitter` 的抖动位、`X-Request-Id` 的随机位、
///           WebSocket 掩码与 multipart 边界的字节都过这一层）。判据分两类：
///           ① **纯映射逐值断言**（`unit_from_u64`）——这一处不碰熵源，故可以钉到具体数值：取的是高 53 位
///           除以 `2^53`，低 11 位一律不参与（`0x7FF` 折成 `0.0`、`0x800` 折成最小步长 `2^-53`，两位一起
///           把移位宽度钉死），`1 << 63` 恰为 `0.5`，全 1 折成可表示的最大小于一且**取不到 `1.0`**
///           （`RetryPolicy::jitter` 的公共契约是「等待在 `[算得值, 算得值 × (1 + jitter)]` 内取值」，
///           折算若除以 `2^64` 之外的错基数，取值域会被压窄或越界，这两条断言正是那两型错误的猎枪）；
///           ② **形状与分布断言**（`random_unit` / `random_u64`）——真随机不能逐值断言，故沿用退场那枚序列
///           的三条判据：十等分桶每桶命中、上下两端都够得到、64 位取值的高低两半都出现过 1，外加进程内
///           逐枚互异（122 位空间下 1000 枚相撞的概率可忽略，这条在旧序列上做不到——那枚序列只有 31 位
///           有效段，1000 枚里撞一次是必然）。
///           两处刻意不作判据：`random_u64` 的字节序（均匀熵下两种拼法无行为差异，钉它只会把实现细节写成
///           契约）；取不到熵时的降级分支（要的是没有熵源的环境，本用例跑到的每条腿都有熵，那一行的 WARN
///           也没有可断言的落点——仓内无日志捕获面）。降级交出的 `0.0` 由 `unit_from_u64` 那条纯断言间接
///           覆盖：`0.0` 正是「不打散」的那个值。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "aurora/net/detail/random_draw.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_random_draw {
namespace {

using aurora::net::detail::random_u64;
using aurora::net::detail::random_unit;
using aurora::net::detail::unit_from_u64;

/// @brief `2^-53`：`double` 在一附近能分辨的最小步长，也是本映射的最小非零输出。
constexpr double AURORA_UNIT_STEP = 1.0 / 9007199254740992.0;

/// @brief 取若干枚 `[0, 1)` 取值。
/// @param count 枚数。
/// @return 取值表。
[[nodiscard]] auto draw_units(std::size_t count) -> std::vector<double> {
    std::vector<double> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_back(random_unit());
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(unit_mapping_uses_exactly_the_high_fifty_three_bits) {
    AURORA_TEST_CHECK_EQ(unit_from_u64(UINT64_C(0)), 0.0);
    // 低 11 位不参与折算：这两枚一起把移位宽度钉死——右移多一位时 `0x800` 也折成 `0.0`。
    AURORA_TEST_CHECK_EQ(unit_from_u64(UINT64_C(0x7FF)), 0.0);
    AURORA_TEST_CHECK_EQ(unit_from_u64(UINT64_C(0x800)), AURORA_UNIT_STEP);
    // 最高位单独置起恰为一半：除以 2^53 与除以 2^64 的分岔在这里可见（后者会交出 `2^-1 × 2^-11`）。
    AURORA_TEST_CHECK_EQ(unit_from_u64(UINT64_C(1) << 63), 0.5);
    // 全 1 折成可表示的最大小于一：上界取不到，故 `[算得值, 算得值 × (1 + jitter)]` 的乘子恒在半开区间。
    AURORA_TEST_CHECK_EQ(unit_from_u64(~UINT64_C(0)), 9007199254740991.0 * AURORA_UNIT_STEP);
    AURORA_TEST_CHECK_LT(unit_from_u64(~UINT64_C(0)), 1.0);
}

AURORA_TEST_CASE(unit_mapping_is_monotonic_within_the_half_open_interval) {
    // 单调非降是「同一枚序列的取值越大则乘子越大」的读法保证；越界（≥ 1.0）则会让抖动契约反向。
    constexpr std::array<std::uint64_t, 6> ladder{UINT64_C(0),       UINT64_C(0x800),   UINT64_C(1) << 40,
                                                  UINT64_C(1) << 62, UINT64_C(1) << 63, ~UINT64_C(0)};
    double previous = 0.0;
    for (const std::uint64_t value : ladder) {
        const double unit = unit_from_u64(value);
        AURORA_TEST_CHECK_GE(unit, previous);
        AURORA_TEST_CHECK(unit >= 0.0 && unit < 1.0);
        previous = unit;
    }
}

AURORA_TEST_CASE(drawn_units_spread_the_whole_half_open_interval) {
    const auto values = draw_units(2000);
    // 「铺满整个半开区间」按十等分桶命中判：取值域若被压成 `[0, 0.5)`（错基数那一型），上半五桶全空。
    constexpr int buckets = 10;
    std::vector<int> coverage(static_cast<std::size_t>(buckets), 0);
    double least = values.front();
    double most = values.front();
    for (const double value : values) {
        AURORA_TEST_CHECK(value >= 0.0 && value < 1.0);
        const int bucket = static_cast<int>(value * static_cast<double>(buckets));
        // 右端兜底，绝不越界：`value` 恰为 `1.0` 时 bucket 会顶到 10，夹回末桶。
        const auto index = static_cast<std::size_t>(std::clamp(bucket, 0, buckets - 1));
        ++coverage[index];
        least = std::min(least, value);
        most = std::max(most, value);
    }
    for (const int hits : coverage) {
        AURORA_TEST_CHECK(hits > 0);
    }
    AURORA_TEST_CHECK_LT(least, 0.05);
    AURORA_TEST_CHECK_GT(most, 0.95);
}

AURORA_TEST_CASE(drawn_sixty_four_bit_values_are_distinct_within_one_process) {
    std::set<std::uint64_t> seen;
    for (std::size_t index = 0; index < 1000; ++index) {
        const auto value = random_u64();
        AURORA_TEST_CHECK(static_cast<bool>(value));
        if (!value) {
            return;  // 本腿无熵：后续互异断言没有可判的对象，如实停在这一条已记下的失败之后
        }
        AURORA_TEST_CHECK_EQ(seen.insert(value.value()).second, true);
    }
    AURORA_TEST_CHECK_EQ(seen.size(), std::size_t{1000});
}

AURORA_TEST_CASE(a_sixty_four_bit_draw_uses_both_halves) {
    // 拼接只写进一枚 32 位有效段时，取值会全部挤在半边：用「最高位与最低位都出现过 1」把两半都钉住。
    bool any_high_bit = false;
    bool any_low_bit = false;
    for (std::size_t index = 0; index < 200; ++index) {
        const auto value = random_u64();
        AURORA_TEST_CHECK(static_cast<bool>(value));
        if (!value) {
            return;
        }
        any_high_bit = any_high_bit || (value.value() & (UINT64_C(1) << 63)) != 0;
        any_low_bit = any_low_bit || (value.value() & UINT64_C(1)) != 0;
    }
    AURORA_TEST_CHECK(any_high_bit);
    AURORA_TEST_CHECK(any_low_bit);
}

}  // namespace aurora::test_cases::utest_net_random_draw
