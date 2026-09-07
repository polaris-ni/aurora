/// 测试类型: unit
/// 目标单元: include/aurora/app/perf_overlay.h
/// 测试说明: 帧统计 FrameStats（滑动窗口 FPS/均值/百分位/掉帧/hitch/idle 检测/相位均值/reset）与 PerfOverlay 自描述单元测试

#include <string>

#include "aurora/app/perf_overlay.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_perf_overlay {

AURORA_TEST() {
    FrameStats &s = FrameStats::instance();

    // ---- 1. reset 后为空窗，帧预算回到默认 ----
    {
        s.reset();
        AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{0});
        AURORA_TEST_CHECK_EQ(s.total_frames(), std::size_t{0});
        AURORA_TEST_CHECK_NEAR(s.fps(), 0.0, 1e-9);
        AURORA_TEST_CHECK_NEAR(s.avg_frame_ms(), 0.0, 1e-9);
        AURORA_TEST_CHECK_NEAR(s.frame_budget_ms(), 16.67, 1e-6);
    }

    // ---- 2. 连续记录等长帧：均值 / FPS / 最差帧一致 ----
    {
        s.reset();
        for (int i = 0; i < 3; ++i) {
            s.record(0.01);  // 10ms/帧
        }
        AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{3});
        AURORA_TEST_CHECK_NEAR(s.avg_frame_ms(), 10.0, 1e-3);
        AURORA_TEST_CHECK_NEAR(s.fps(), 100.0, 1e-2);
        AURORA_TEST_CHECK_NEAR(s.worst_frame_ms(), 10.0, 1e-3);
    }

    // ---- 3. 非正 dt 被忽略 ----
    {
        s.reset();
        s.record(0.0);
        s.record(-1.0);
        AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{0});
    }

    // ---- 4. 掉帧 / hitch：超预算记 dropped，超两倍预算再记 hitch ----
    {
        s.reset();
        s.record(0.02);  // 20ms > 16.67（掉帧，非 hitch）
        s.record(0.04);  // 40ms > 33.34（掉帧 + hitch）
        AURORA_TEST_CHECK_EQ(s.dropped_frame_count(), std::size_t{2});
        AURORA_TEST_CHECK_EQ(s.hitch_count(), std::size_t{1});
        AURORA_TEST_CHECK_NEAR(s.dropped_frame_ratio(), 1.0, 1e-9);
    }

    // ---- 5. idle 段：远超阈值的 dt 只计计数器、不入窗口 ----
    {
        s.reset();
        s.record(0.2);  // 200ms > 100ms 阈值
        AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{0});
        AURORA_TEST_CHECK_GT(s.idle_frame_count(), std::size_t{0});
        AURORA_TEST_CHECK_GT(s.total_frames(), std::size_t{0});
    }

    // ---- 6. 百分位单调 + frame_at（0=最新） ----
    {
        s.reset();
        s.record(0.01);  // 10ms
        s.record(0.02);  // 20ms
        s.record(0.03);  // 30ms
        AURORA_TEST_CHECK_NEAR(s.percentile_ms(0.0), 10.0, 1.0);
        AURORA_TEST_CHECK_NEAR(s.percentile_ms(1.0), 30.0, 1.0);
        AURORA_TEST_CHECK_NEAR(s.frame_at(0), 0.03, 1e-6);  // 最新帧 30ms
        AURORA_TEST_CHECK(s.percentile_ms(0.0) <= s.percentile_ms(1.0));
    }

    // ---- 7. 相位计时均值 + 帧预算覆写 ----
    {
        s.reset();
        s.record_phases(2.0, 3.0, 5.0);
        AURORA_TEST_CHECK_NEAR(s.avg_layout_ms(), 2.0, 1e-6);
        AURORA_TEST_CHECK_NEAR(s.avg_paint_ms(), 3.0, 1e-6);
        AURORA_TEST_CHECK_NEAR(s.avg_present_ms(), 5.0, 1e-6);
        s.set_frame_budget_ms(8.0);
        AURORA_TEST_CHECK_NEAR(s.frame_budget_ms(), 8.0, 1e-6);
    }

    // ---- 8. PerfOverlay：类型名 / 自描述 / 统计行可读 ----
    {
        PerfOverlay ov;
        AURORA_TEST_CHECK_EQ(std::string(ov.type_name()), std::string("PerfOverlay"));
        const WidgetDescriptor d = PerfOverlay::describe_static();
        AURORA_TEST_CHECK_EQ(std::string(d.name), std::string("PerfOverlay"));
        AURORA_TEST_CHECK(!PerfOverlay::stats_line1().empty());
        AURORA_TEST_CHECK(!PerfOverlay::stats_line2().empty());
    }

    s.reset();  // 复位进程级单例，避免影响其他用例（虽然 runner 进程隔离，仍守洁净）
}

}  // namespace aurora::test_cases::utest_perf_overlay
