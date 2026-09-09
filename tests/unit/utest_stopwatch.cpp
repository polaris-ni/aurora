/// 测试类型: unit
/// 目标单元: include/aurora/perf/stopwatch.h
/// 测试说明: 覆盖 Stopwatch——构造即计时、读数非负且单调、ms/us/ns 三种单位读数、
/// reset 重启归零、lap 分段取值并重启、静态 now_ms 单调原点。
/// 时间断言纪律：只断言非负、单调、重启归零等确定性质，不依赖精确计时（忙等制造确定耗时）。

#include <cmath>
#include <cstdint>

#include "aurora/perf/stopwatch.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_stopwatch {

namespace {

/// @brief 忙等直到秒表读数超过 min_ms，返回当时的读数（制造确定耗时，不依赖 sleep 精度）。
auto busy_wait_ms(Stopwatch& sw, double min_ms) -> double {
    double ms = 0.0;
    while (ms < min_ms) {
        ms = sw.elapsed_ms();
    }
    return ms;
}

}  // namespace

AURORA_TEST_CASE(construction_starts_clock_with_nonnegative_readings) {
    // 构造即开始计时：三种单位的读数都非负且有限。
    const Stopwatch sw;
    AURORA_TEST_CHECK_GE(sw.elapsed_ms(), 0.0);
    AURORA_TEST_CHECK_GE(sw.elapsed_us(), 0.0);
    AURORA_TEST_CHECK_GE(sw.elapsed_ns(), 0);
    AURORA_TEST_CHECK_TRUE(std::isfinite(sw.elapsed_ms()));
    AURORA_TEST_CHECK_TRUE(std::isfinite(sw.elapsed_us()));
}

AURORA_TEST_CASE(elapsed_readings_are_monotonic) {
    // 同一实例连续读取：读数不回退（单调时钟）；us 读数与 ms 读数同刻度换算一致。
    Stopwatch sw;
    const double ms_first = sw.elapsed_ms();
    const double ms_second = sw.elapsed_ms();
    AURORA_TEST_CHECK_GE(ms_second, ms_first);
    // us 后读，读数（按换算）不得小于先读的 ms 值换算结果（减一个浮点容差防舍入）。
    const double us_later = sw.elapsed_us();
    AURORA_TEST_CHECK_GE(us_later, (ms_second * 1000.0) - 1e-6);
    // ns 整数读数与 ms 同源，同样单调非负。
    AURORA_TEST_CHECK_GE(sw.elapsed_ns(), 0);
}

AURORA_TEST_CASE(reset_restarts_from_near_zero) {
    // 累计一段确定耗时（> 2ms）后 reset：新读数从近零重新开始，且小于重置前的读数。
    Stopwatch sw;
    const double before = busy_wait_ms(sw, 2.0);
    AURORA_TEST_CHECK_GE(before, 2.0);

    sw.reset();
    const double after = sw.elapsed_ms();
    AURORA_TEST_CHECK_GE(after, 0.0);
    AURORA_TEST_CHECK_LT(after, before);
}

AURORA_TEST_CASE(lap_returns_segment_duration_and_restarts) {
    // lap 返回本段耗时（> 忙等下限），并把起点搬到当前时刻：lap 后立即读数远小于段值。
    Stopwatch phase;
    busy_wait_ms(phase, 2.0);
    const double segment_ms = phase.lap_ms();
    AURORA_TEST_CHECK_GE(segment_ms, 2.0);

    const double after_lap = phase.elapsed_ms();
    AURORA_TEST_CHECK_GE(after_lap, 0.0);
    AURORA_TEST_CHECK_LT(after_lap, segment_ms);

    // 第二段继续分段计时同样成立：再忙等一小段，lap 读数仍非负。
    const double second_segment = busy_wait_ms(phase, 1.0);
    AURORA_TEST_CHECK_GE(second_segment, 1.0);
    AURORA_TEST_CHECK_GE(phase.lap_ms(), 1.0);
}

AURORA_TEST_CASE(elapsed_ns_is_nonnegative_and_grows_with_us) {
    // ns 整数读数与 us 读数同源：换算后不小于 us 换算值减一个 cast 截断容差。
    Stopwatch sw;
    busy_wait_ms(sw, 1.0);
    const double us = sw.elapsed_us();
    const std::int64_t ns = sw.elapsed_ns();
    AURORA_TEST_CHECK_GE(ns, 0);
    AURORA_TEST_CHECK_GE(static_cast<double>(ns), (us * 1000.0) - 1000.0);
}

AURORA_TEST_CASE(now_ms_monotonic_and_nonnegative) {
    // 静态进程原点：首次使用后 now_ms 非负，且后续读取不回退。
    const double first = Stopwatch::now_ms();
    AURORA_TEST_CHECK_GE(first, 0.0);
    const double second = Stopwatch::now_ms();
    AURORA_TEST_CHECK_GE(second, first);
}

}  // namespace aurora::test_cases::utest_stopwatch
