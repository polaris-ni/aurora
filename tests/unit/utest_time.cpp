/// 测试类型: unit
/// 目标单元: include/aurora/core/time.h
/// 测试说明: 覆盖 current_timestamp 的毫秒级 Unix 时间戳量级、连续调用非递减、返回类型契约与非绘制上下文可调用性

#include <cstdint>
#include <type_traits>

#include "aurora/core/time.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_time {

namespace au = aurora;

/// @brief 时间戳为毫秒级 Unix epoch：落在 2001-09 之后、2100 年之前（系统时钟决定的宽松窗口）。
AURORA_TEST_CASE(timestamp_is_millisecond_epoch_in_sane_window) {
    const auto t = au::current_timestamp();
    AURORA_TEST_CHECK_GT(t, 1'000'000'000'000ULL);  // > 2001-09-09（毫秒）
    AURORA_TEST_CHECK_LT(t, 4'102'444'800'000ULL);  // < 2100-01-01（毫秒）
}

/// @brief 连续两次调用非递减（同一进程内系统时钟不回退）。
AURORA_TEST_CASE(consecutive_calls_are_non_decreasing) {
    const auto first = au::current_timestamp();
    const auto second = au::current_timestamp();
    AURORA_TEST_CHECK_GE(second, first);
}

/// @brief 返回类型契约：std::uint64_t（毫秒数足够 5.8 亿年不溢出）。
AURORA_TEST_CASE(return_type_is_uint64) {
    static_assert(std::is_same_v<decltype(au::current_timestamp()), std::uint64_t>);
    AURORA_TEST_CHECK_EQ(sizeof(decltype(au::current_timestamp())), sizeof(std::uint64_t));
}

/// @brief 短促多次调用全部落在首个读数起的宽裕窗口内且非递减（无 sleep，确定性成立）。
/// @note AURORA_ENABLE_DEBUG 构建下每次调用都会检查渲染纯度守卫（g_paint_depth == 0）：
///       本用例能正常返回即隐含验证了「非绘制上下文读时钟不断言」。
AURORA_TEST_CASE(burst_of_calls_stays_within_window) {
    const auto first = au::current_timestamp();
    for (int i = 0; i < 5; ++i) {
        const auto t = au::current_timestamp();
        AURORA_TEST_CHECK_GE(t, first);
        AURORA_TEST_CHECK_LE(t - first, 60'000ULL);  // 无 sleep，60s 窗口足够宽裕
    }
}

}  // namespace aurora::test_cases::utest_time
