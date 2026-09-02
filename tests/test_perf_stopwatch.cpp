// test_perf_stopwatch.cpp — `aurora::Stopwatch` 单测。
//
// 计时类断言的通病是「在忙碌机器上偶发 flaky」。本文件只断言**口径不变量**，
// 不断言绝对耗时：
//   - 单调不减、非负；
//   - 三种单位读数彼此自洽（us ≈ ms×1000、ns ≈ us×1000）；
//   - reset / lap 的语义（归零、分段不重叠）；
//   - now_ms 共享同一时间原点、跨实例可比较。
// 唯一涉及真实时长的断言用「忙等 ≥ 目标值」的方式给出单边下界，不设上界。
#include <cmath>
#include <cstdint>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::Stopwatch;

namespace {

/// @brief 忙等至少 `target_ms` 毫秒（不用 sleep：调度粒度会让「至少」变成「大概」）。
auto busy_wait_ms(double target_ms) -> void {
    const Stopwatch sw;
    volatile double sink = 0.0;
    while (sw.elapsed_ms() < target_ms) {
        sink += 1.0; // 防止循环被优化掉
    }
    (void)sink;
}

// ---- Test 1: 构造即计时，读数非负且单调不减 ----
auto test_monotonic_nonnegative() -> void {
    const Stopwatch sw;
    const double a = sw.elapsed_ms();
    busy_wait_ms(1.0);
    const double b = sw.elapsed_ms();

    AURORA_TEST_CHECK_MSG(a >= 0.0, "Test1: elapsed_ms() non-negative");
    AURORA_TEST_CHECK_MSG(b >= a, "Test1: elapsed_ms() monotonic non-decreasing");
}

// ---- Test 2: 三种单位读数自洽 ----
auto test_unit_consistency() -> void {
    const Stopwatch sw;
    busy_wait_ms(2.0);

    // 三次读数取自不同时刻，故只能校验数量级关系而非严格相等：
    // 先读 ns（最早）再读 us、ms（最晚），后者必然 ≥ 前者换算值。
    const std::int64_t ns = sw.elapsed_ns();
    const double us = sw.elapsed_us();
    const double ms = sw.elapsed_ms();

    AURORA_TEST_CHECK_MSG(us >= static_cast<double>(ns) / 1000.0,
                          "Test2: us reading >= ns-converted value read earlier");
    AURORA_TEST_CHECK_MSG(ms >= us / 1000.0, "Test2: ms reading >= us-converted value read earlier");
    // 上界放宽到 2 倍：忙等 2ms 内三次读数的间隔远小于 2ms，不可能翻倍。
    AURORA_TEST_CHECK_MSG(ms <= ((us / 1000.0) * 2.0) + 1.0,
                          "Test2: ms and us-converted value same order of magnitude");
    AURORA_TEST_CHECK_MSG(ns > 0, "Test2: ns reading positive after 2ms busy wait");
}

// ---- Test 3: 忙等 5ms 后读数确实 ≥ 5ms（单边下界，不设上界）----
auto test_measures_real_elapsed() -> void {
    const Stopwatch sw;
    busy_wait_ms(5.0);
    const double ms = sw.elapsed_ms();

    AURORA_TEST_CHECK_MSG(ms >= 5.0, "Test3: elapsed_ms() >= 5.0 after 5ms busy wait");
    AURORA_TEST_CHECK_MSG(sw.elapsed_us() >= 5000.0, "Test3: elapsed_us() >= 5000 after 5ms busy wait");
}

// ---- Test 4: reset 归零 ----
auto test_reset() -> void {
    Stopwatch sw;
    busy_wait_ms(5.0);
    const double before = sw.elapsed_ms();
    sw.reset();
    const double after = sw.elapsed_ms();

    AURORA_TEST_CHECK_MSG(before >= 5.0, "Test4: accumulated >= 5ms before reset");
    AURORA_TEST_CHECK_MSG(after < before, "Test4: reading after reset smaller than before");
    AURORA_TEST_CHECK_MSG(after < 5.0, "Test4: reading after reset falls within 5ms");
}

// ---- Test 5: lap_ms 分段不重叠（各段之和 ≤ 总时长）----
auto test_lap_segments() -> void {
    const Stopwatch total;
    Stopwatch phase;

    busy_wait_ms(3.0);
    const double seg1 = phase.lap_ms();
    busy_wait_ms(3.0);
    const double seg2 = phase.lap_ms();

    const double whole = total.elapsed_ms();

    AURORA_TEST_CHECK_MSG(seg1 >= 3.0, "Test5: segment 1 >= 3ms");
    AURORA_TEST_CHECK_MSG(seg2 >= 3.0, "Test5: segment 2 >= 3ms");
    // lap 是「取读数并重置」，两段互不重叠，之和不可能超过外层总时长（+ 容差）。
    AURORA_TEST_CHECK_MSG(seg1 + seg2 <= whole + 1.0, "Test5: segment sum does not exceed total (no overlap)");
}

// ---- Test 6: now_ms 共享进程级时间原点，跨实例可比较 ----
auto test_now_ms_shared_epoch() -> void {
    const double t0 = Stopwatch::now_ms();
    busy_wait_ms(3.0);
    const double t1 = Stopwatch::now_ms();

    AURORA_TEST_CHECK_MSG(t0 >= 0.0, "Test6: now_ms() non-negative (relative to process origin)");
    AURORA_TEST_CHECK_MSG(t1 > t0, "Test6: now_ms() strictly increasing");
    AURORA_TEST_CHECK_MSG(t1 - t0 >= 3.0, "Test6: now_ms() delta reflects real elapsed time");
}

// ---- Test 7: 多实例互不干扰 ----
auto test_instances_independent() -> void {
    const Stopwatch older;
    busy_wait_ms(4.0);
    const Stopwatch newer;
    busy_wait_ms(1.0);

    AURORA_TEST_CHECK_MSG(older.elapsed_ms() > newer.elapsed_ms(),
                          "Test7: earlier-constructed instance reads larger (no shared state)");
    AURORA_TEST_CHECK_MSG(older.elapsed_ms() >= 5.0, "Test7: earlier-constructed instance accumulated >= 5ms");
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_perf_stopwatch ===\n");

    test_monotonic_nonnegative();
    test_unit_consistency();
    test_measures_real_elapsed();
    test_reset();
    test_lap_segments();
    test_now_ms_shared_epoch();
    test_instances_independent();
}
