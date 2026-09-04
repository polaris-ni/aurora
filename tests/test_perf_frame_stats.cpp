// perf_frame_stats_test.cpp — 覆盖 FrameStats 增强 API 的单测。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// perf/perf_session.h(FrameStats 滑动窗口 + PerfSession 全量聚合/p99/jitter)。

#include <cmath>
#include <cstdio>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::FrameStats;

// ---- Test 1: 基本 record + fps/avg ----
static void test_basic_record_fps() {
    auto &fs = FrameStats::instance();
    fs.reset();

    for (int i = 0; i < 60; ++i) {
        fs.record(0.01667);
    }

    AURORA_TEST_CHECK_MSG(near_d(fs.fps(), 60.0, 1.0), "Test1: fps() ~60 after 60 frames @16.67ms");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_frame_ms(), 16.67, 0.1), "Test1: avg_frame_ms() ~16.67");
}

// ---- Test 2: 环形缓冲区溢出 ----
static void test_ring_buffer_overflow() {
    auto &fs = FrameStats::instance();
    fs.reset();

    for (int i = 0; i < 200; ++i) {
        fs.record(0.01);
    }

    AURORA_TEST_CHECK_MSG(fs.window_size() == 128, "Test2: window_size() == 128 after 200 frames");
    AURORA_TEST_CHECK_MSG(fs.total_frames() == 200, "Test2: total_frames() == 200");
}

// ---- Test 3: worst_frame_ms ----
static void test_worst_frame_ms() {
    auto &fs = FrameStats::instance();
    fs.reset();

    fs.record(0.016);  // 16ms
    fs.record(0.050);  // 50ms (slow frame)
    fs.record(0.016);  // 16ms

    AURORA_TEST_CHECK_MSG(near_d(fs.worst_frame_ms(), 50.0, 0.1), "Test3: worst_frame_ms() ~50.0");
}

// ---- Test 4: jitter_ms ----
static void test_jitter_ms() {
    auto &fs = FrameStats::instance();
    fs.reset();

    for (int i = 0; i < 64; ++i) {
        fs.record(i % 2 == 0 ? 0.016 : 0.033);
    }

    AURORA_TEST_CHECK_MSG(fs.jitter_ms() > 0.0, "Test4: jitter_ms() > 0 for alternating frame times");
}

// ---- Test 5: percentile_ms ----
static void test_percentile_ms() {
    auto &fs = FrameStats::instance();
    fs.reset();

    // 记录 100 帧，帧时间从 1ms 到 100ms 递增（避免超过 IDLE_THRESHOLD_MS=100ms）
    for (int i = 0; i < 100; ++i) {
        fs.record(0.001 + (i * 0.001));  // 1ms, 2ms, ..., 100ms
    }

    // P50 应接近 50ms，P99 应接近 100ms
    AURORA_TEST_CHECK_MSG(near_d(fs.percentile_ms(0.50), 50.5, 1.0), "Test5: P50 ~50ms");
    AURORA_TEST_CHECK_MSG(near_d(fs.percentile_ms(0.99), 100.0, 1.0), "Test5: P99 ~100ms");
}

// ---- Test 6: dropped_frame_count + hitch_count ----
static void test_dropped_and_hitch() {
    auto &fs = FrameStats::instance();
    fs.reset();
    fs.set_frame_budget_ms(16.67);

    fs.record(0.010);  // 10ms - OK
    fs.record(0.020);  // 20ms - dropped (> 16.67ms)
    fs.record(0.040);  // 40ms - hitch (> 33.34ms)
    fs.record(0.010);  // 10ms - OK

    AURORA_TEST_CHECK_MSG(fs.dropped_frame_count() == 2, "Test6: dropped_frame_count() == 2");
    AURORA_TEST_CHECK_MSG(fs.hitch_count() == 1, "Test6: hitch_count() == 1");
}

// ---- Test 7: record_idle ----
static void test_record_idle() {
    auto &fs = FrameStats::instance();
    fs.reset();

    fs.record(0.016);
    fs.record_idle();
    fs.record_idle();

    AURORA_TEST_CHECK_MSG(fs.idle_frame_count() == 2, "Test7: idle_frame_count() == 2");
    AURORA_TEST_CHECK_MSG(fs.total_frames() == 3, "Test7: total_frames() == 3");
    AURORA_TEST_CHECK_MSG(fs.window_size() == 1, "Test7: window_size() == 1 (only 1 frame in buffer)");
}

// ---- Test 8: idle 自动检测（dt > 100ms）----
static void test_auto_idle_detection() {
    auto &fs = FrameStats::instance();
    fs.reset();

    fs.record(0.016);  // 16ms - normal
    fs.record(0.200);  // 200ms - auto idle (200/16.67 ≈ 12 frames skipped)

    AURORA_TEST_CHECK_MSG(fs.idle_frame_count() > 0, "Test8: idle_frame_count() > 0 after 200ms gap");
    AURORA_TEST_CHECK_MSG(fs.window_size() == 1, "Test8: window_size() == 1 (idle frames not in buffer)");
}

// ---- Test 9: record_phases ----
static void test_record_phases() {
    auto &fs = FrameStats::instance();
    fs.reset();

    fs.record_phases(2.0, 8.0, 4.0);
    fs.record_phases(3.0, 7.0, 5.0);

    AURORA_TEST_CHECK_MSG(near_d(fs.avg_layout_ms(), 2.5, 0.01), "Test9: avg_layout_ms() ~2.5");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_paint_ms(), 7.5, 0.01), "Test9: avg_paint_ms() ~7.5");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_present_ms(), 4.5, 0.01), "Test9: avg_present_ms() ~4.5");
}

// ---- Test 10: reset ----
static void test_reset() {
    auto &fs = FrameStats::instance();

    fs.record(0.016);
    fs.record_idle();
    fs.record_phases(1.0, 2.0, 3.0);
    fs.reset();

    AURORA_TEST_CHECK_MSG(near_d(fs.fps(), 0.0, 0.001), "Test10: fps() == 0 after reset");
    AURORA_TEST_CHECK_MSG(fs.window_size() == 0, "Test10: window_size() == 0 after reset");
    AURORA_TEST_CHECK_MSG(fs.total_frames() == 0, "Test10: total_frames() == 0 after reset");
    AURORA_TEST_CHECK_MSG(fs.idle_frame_count() == 0, "Test10: idle_frame_count() == 0 after reset");
    AURORA_TEST_CHECK_MSG(fs.dropped_frame_count() == 0, "Test10: dropped_frame_count() == 0 after reset");
    AURORA_TEST_CHECK_MSG(fs.hitch_count() == 0, "Test10: hitch_count() == 0 after reset");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_layout_ms(), 0.0, 0.001), "Test10: avg_layout_ms() == 0 after reset");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_paint_ms(), 0.0, 0.001), "Test10: avg_paint_ms() == 0 after reset");
    AURORA_TEST_CHECK_MSG(near_d(fs.avg_present_ms(), 0.0, 0.001), "Test10: avg_present_ms() == 0 after reset");
}

// ---- Test 11: frame_at ----
static void test_frame_at() {
    auto &fs = FrameStats::instance();
    fs.reset();

    fs.record(0.010);  // frame 0
    fs.record(0.020);  // frame 1
    fs.record(0.030);  // frame 2

    AURORA_TEST_CHECK_MSG(near_d(fs.frame_at(0), 0.030, 0.0001), "Test11: frame_at(0) ~0.030 (newest)");
    AURORA_TEST_CHECK_MSG(near_d(fs.frame_at(1), 0.020, 0.0001), "Test11: frame_at(1) ~0.020");
    AURORA_TEST_CHECK_MSG(near_d(fs.frame_at(2), 0.010, 0.0001), "Test11: frame_at(2) ~0.010 (oldest)");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== perf_frame_stats_test ===\n");

    test_basic_record_fps();
    test_ring_buffer_overflow();
    test_worst_frame_ms();
    test_jitter_ms();
    test_percentile_ms();
    test_dropped_and_hitch();
    test_record_idle();
    test_auto_idle_detection();
    test_record_phases();
    test_reset();
    test_frame_at();
}
