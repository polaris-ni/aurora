/// 测试类型: unit
/// 目标单元: include/aurora/core/time.h
/// 测试说明: current_timestamp 毫秒时间戳的量纲、单调性与绘制纯度守卫单元测试

#include <chrono>
#include <cstdint>

#include "aurora/core/time.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_time {

AURORA_TEST() {
    // ---- 1. 返回毫秒级系统时间戳（与 system_clock 同量纲） ----
    {
        const auto before =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const auto ts = current_timestamp();
        const auto after =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());

        AURORA_TEST_CHECK_GE(ts, before);
        AURORA_TEST_CHECK_LE(ts, after);
    }

    // ---- 2. 单调不减：连续两次采样后者不小于前者 ----
    {
        const auto a = current_timestamp();
        const auto b = current_timestamp();
        AURORA_TEST_CHECK_GE(b, a);
    }

    // ---- 3. 量纲校验：落在合理区间（2000 年之后、且不是微秒/纳秒量级） ----
    {
        const auto ts = current_timestamp();
        // 2000-01-01T00:00:00Z 约 946684800000 ms
        AURORA_TEST_CHECK_GT(ts, 946684800000ULL);
        // 若是微秒量纲会 > 1e15，纳秒会 > 1e17
        AURORA_TEST_CHECK_LT(ts, 100000000000000ULL);
    }
}

}  // namespace aurora::test_cases::utest_time
