/// 测试类型: unit
/// 目标单元: include/aurora/app/perf_overlay.h
/// 测试说明: 覆盖 FrameStats 滑动窗口统计（fps/avg/worst/百分位/jitter、掉帧/hitch/idle、
/// 分阶段计时、唤醒观测、非正 dt 忽略）、停帧陈旧语义（is_stale / stale_duration_ms 的
/// 累加·清零·越阈，及 `fps()` 保持末值不归零）与 PerfOverlay 开关语义、统计行文本的
/// 采样中/就绪/陈旧标注语义、fps 告警色阈值、布局填充与自描述

#include <string>

#include "aurora/app/perf_overlay.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_perf_overlay {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(frame_stats_window_metrics) {
    auto &s = FrameStats::instance();
    s.reset();

    // 空窗口：指标安全回退 0，不产生 NaN/除零。
    AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(s.fps(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_frame_ms(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.worst_frame_ms(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.jitter_ms(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.percentile_ms(0.99), 0.0, 1e-4);

    // 记录 10ms 与 20ms 两帧：fps = 2 / 0.03s ≈ 66.67。
    s.record(0.010);
    s.record(0.020);
    AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{2});
    AURORA_TEST_CHECK_NEAR(s.fps(), 66.6667, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_frame_ms(), 15.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.worst_frame_ms(), 20.0, 1e-4);
    // 百分位（两样本线性插值）：p0 = 最小值，p99 ≈ 19.9，p1 = 最大值。
    AURORA_TEST_CHECK_NEAR(s.percentile_ms(0.0), 10.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.percentile_ms(0.99), 19.9, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.percentile_ms(1.0), 20.0, 1e-4);
    // 两帧差异 → 标准差 = 5ms。
    AURORA_TEST_CHECK_NEAR(s.jitter_ms(), 5.0, 1e-4);
}

AURORA_TEST_CASE(frame_stats_dropped_hitch_and_idle) {
    auto &s = FrameStats::instance();
    s.reset();

    // 20ms：超单倍预算、未超双倍 → 仅记掉帧。
    s.record(0.020);
    AURORA_TEST_CHECK_EQ(s.dropped_frame_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(s.hitch_count(), std::size_t{0});
    // 80ms：超双倍预算 → 掉帧 + hitch。
    s.record(0.080);
    AURORA_TEST_CHECK_EQ(s.dropped_frame_count(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(s.hitch_count(), std::size_t{1});
    // 目前全部窗口内样本均为掉帧 → 比例 1。
    AURORA_TEST_CHECK_NEAR(s.dropped_frame_ratio(), 1.0, 1e-4);

    // idle 段：500ms > 100ms 阈值 → 按预算折算跳帧，仅递增计数器、不入环形缓冲。
    const auto window_before = s.window_size();
    const auto total_before = s.total_frames();
    s.record(0.5);
    AURORA_TEST_CHECK_GT(s.idle_frame_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(s.window_size(), window_before);
    AURORA_TEST_CHECK_EQ(s.total_frames(), total_before + s.idle_frame_count());

    // record_idle：显式记一次 idle 跳帧。
    const auto idle_before = s.idle_frame_count();
    s.record_idle();
    AURORA_TEST_CHECK_EQ(s.idle_frame_count(), idle_before + 1);
}

AURORA_TEST_CASE(frame_stats_ignores_nonpositive_dt) {
    auto &s = FrameStats::instance();
    s.reset();

    s.record(0.0);
    s.record(-0.016);
    AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(s.total_frames(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(s.fps(), 0.0, 1e-4);
}

AURORA_TEST_CASE(frame_stats_staleness_marks_stopped_rendering) {
    auto &s = FrameStats::instance();
    s.reset();

    // 初始：无空闲累计，不算陈旧。
    AURORA_TEST_CHECK_FALSE(s.is_stale());
    AURORA_TEST_CHECK_NEAR(s.stale_duration_ms(), 0.0, 1e-9);

    // 带 dt 的 record_idle 才累加空闲时长；不带 dt 的兼容路径只计计数器。
    s.record_idle();
    AURORA_TEST_CHECK_EQ(s.idle_frame_count(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(s.is_stale());

    s.record(1.0 / 60.0);
    s.record(1.0 / 60.0);
    const double fps_when_active = s.fps();
    AURORA_TEST_CHECK_FALSE(s.is_stale());

    // 低于阈值的空闲：动画中偶发跳过的单帧不该被判成停帧（否则 HUD 数字无谓闪烁）。
    s.record_idle(0.2);
    AURORA_TEST_CHECK_FALSE(s.is_stale());
    AURORA_TEST_CHECK_NEAR(s.stale_duration_ms(), 200.0, 1e-6);

    // 越阈（累计 0.6s ≥ AURORA_FPS_STALE_MS）→ 陈旧；关键：fps() **保持末值**不归零
    // （归零会丢掉「上次活跃帧率」这一排障信息），空闲帧也不进环形缓冲。
    s.record_idle(0.4);
    AURORA_TEST_CHECK_TRUE(s.is_stale());
    AURORA_TEST_CHECK_NEAR(s.stale_duration_ms(), 600.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(s.fps(), fps_when_active, 1e-9);
    AURORA_TEST_CHECK_EQ(s.window_size(), std::size_t{2});

    // 再次渲染 → 立刻恢复新鲜、空闲累计清零。
    s.record(1.0 / 60.0);
    AURORA_TEST_CHECK_FALSE(s.is_stale());
    AURORA_TEST_CHECK_NEAR(s.stale_duration_ms(), 0.0, 1e-9);

    // dt 偏大的「慢帧」不是停帧：超 idle 阈值只影响帧时间窗口，本帧毕竟渲染过了。
    s.record(0.5);
    AURORA_TEST_CHECK_FALSE(s.is_stale());

    // reset 连同陈旧状态一并清零。
    s.record_idle(1.0);
    AURORA_TEST_CHECK_TRUE(s.is_stale());
    s.reset();
    AURORA_TEST_CHECK_FALSE(s.is_stale());
    AURORA_TEST_CHECK_NEAR(s.stale_duration_ms(), 0.0, 1e-9);
}

AURORA_TEST_CASE(perf_overlay_stats_line1_annotates_stale_fps) {
    auto &s = FrameStats::instance();
    s.reset();
    s.record(1.0 / 60.0);
    s.record(1.0 / 60.0);

    // 活跃期不带 stale 标注，且仍以 FPS 开头（既有解析口径不变）。
    const std::string active_line = PerfOverlay::stats_line1(s);
    AURORA_TEST_CHECK_TRUE(active_line.rfind("FPS 60.0", 0) == 0);
    AURORA_TEST_CHECK_TRUE(active_line.find("stale") == std::string::npos);

    // 停帧后：数值保留（不换成 0 / —），追加空闲时长标注。
    s.record_idle(1.2);
    const std::string stale_line = PerfOverlay::stats_line1(s);
    AURORA_TEST_CHECK_TRUE(stale_line.rfind("FPS 60.0", 0) == 0);
    AURORA_TEST_CHECK_TRUE(stale_line.find("stale 1.2s") != std::string::npos);

    s.reset();
}

AURORA_TEST_CASE(frame_stats_phase_averages) {
    auto &s = FrameStats::instance();
    s.reset();

    AURORA_TEST_CHECK_NEAR(s.avg_layout_ms(), 0.0, 1e-4);
    s.record_phases(1.0, 2.0, 3.0);
    AURORA_TEST_CHECK_NEAR(s.avg_layout_ms(), 1.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_paint_ms(), 2.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_present_ms(), 3.0, 1e-4);

    // 第二个样本参与平均：(1+3)/2 = 2。
    s.record_phases(3.0, 2.0, 1.0);
    AURORA_TEST_CHECK_NEAR(s.avg_layout_ms(), 2.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_paint_ms(), 2.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.avg_present_ms(), 2.0, 1e-4);
}

AURORA_TEST_CASE(frame_stats_wait_observation_and_reset) {
    auto &s = FrameStats::instance();
    s.reset();
    AURORA_TEST_CHECK_EQ(s.wakeup_count(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(s.wakeups_per_sec(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.sleep_ratio(), 0.0, 1e-4);

    s.record_wait(4.0);
    s.record_wait(6.0);
    AURORA_TEST_CHECK_EQ(s.wakeup_count(), std::size_t{2});
    // 墙钟派生指标只验证值域，不做时序敏感断言。
    AURORA_TEST_CHECK_TRUE(s.sleep_ratio() >= 0.0 && s.sleep_ratio() <= 1.0);
    AURORA_TEST_CHECK_TRUE(s.wakeups_per_sec() >= 0.0);

    // reset 清空唤醒观测。
    s.reset();
    AURORA_TEST_CHECK_EQ(s.wakeup_count(), std::size_t{0});
}

AURORA_TEST_CASE(perf_overlay_flags_and_descriptor) {
    PerfOverlay o;
    AURORA_TEST_CHECK_STREQ(o.type_name(), "PerfOverlay");
    // 默认全开；DisplayList 缓存被禁用（HUD 内容每帧变动）。
    AURORA_TEST_CHECK_TRUE(o.visible());
    AURORA_TEST_CHECK_TRUE(o.show_counters());
    AURORA_TEST_CHECK_FALSE(o.can_cache_display_list());

    // 链式 setter 生效。
    o.set_visible(false).set_show_counters(false);
    AURORA_TEST_CHECK_FALSE(o.visible());
    AURORA_TEST_CHECK_FALSE(o.show_counters());
    o.set_visible(true);
    AURORA_TEST_CHECK_TRUE(o.visible());

    // 自描述：单子节点策略 + visible / show_counters 两个属性。
    const auto d = PerfOverlay::describe_static();
    AURORA_TEST_CHECK_STREQ(d.name, "PerfOverlay");
    AURORA_TEST_CHECK_STREQ(d.children_policy, "single");
    bool has_visible = false;
    bool has_show_counters = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "visible") {
            has_visible = true;
        }
        if (std::string{p.name} == "show_counters") {
            has_show_counters = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_visible);
    AURORA_TEST_CHECK_TRUE(has_show_counters);
}

AURORA_TEST_CASE(perf_overlay_stats_lines_and_fps_color) {
    auto &s = FrameStats::instance();
    s.reset();

    // 样本不足（<2 帧）→ 占位提示而非误导性数字。
    AURORA_TEST_CHECK_STREQ(PerfOverlay::stats_line1(), "FPS — (采样中) | P99 — | jitter —");
    AURORA_TEST_CHECK_STREQ(PerfOverlay::stats_line2(), "dropped: 0 | hitch: 0 | idle: 0");
    AURORA_TEST_CHECK_TRUE(PerfOverlay::stats_line3().rfind("wakeups/s: 0.0", 0) == 0);
    AURORA_TEST_CHECK_FALSE(PerfOverlay::stats_line4().empty());
    AURORA_TEST_CHECK_FALSE(PerfOverlay::stats_line5().empty());

    // 两个 1/60s 样本 → FPS 显示为 60.0，且包含 P99 字段。
    s.record(1.0 / 60.0);
    s.record(1.0 / 60.0);
    AURORA_TEST_CHECK_TRUE(PerfOverlay::stats_line1().rfind("FPS 60.0", 0) == 0);
    AURORA_TEST_CHECK_TRUE(PerfOverlay::stats_line1().find("P99") != std::string::npos);

    // fps 告警色阈值：>=55 绿、>=30 黄、否则红。
    const auto green = PerfOverlay::fps_color(55.0);
    AURORA_TEST_CHECK_EQ(green.r, 0);
    AURORA_TEST_CHECK_EQ(green.g, 255);
    const auto yellow = PerfOverlay::fps_color(54.9);
    AURORA_TEST_CHECK_EQ(yellow.r, 255);
    AURORA_TEST_CHECK_EQ(yellow.g, 200);
    AURORA_TEST_CHECK_EQ(PerfOverlay::fps_color(30.0).g, 200);
    const auto red = PerfOverlay::fps_color(29.9);
    AURORA_TEST_CHECK_EQ(red.r, 255);
    AURORA_TEST_CHECK_EQ(red.g, 60);
}

AURORA_TEST_CASE(perf_overlay_layout_fills_and_child_bounds) {
    // 有子节点：自身占满约束上限，子节点铺满自身（原点对齐）。
    auto content = std::make_shared<Text>("x");
    content->width(aurora::Length::fixed(100.0F)).height(aurora::Length::fixed(50.0F));
    PerfOverlay o{Node{content}};
    LayoutEngine::layout(o, bounded(800.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(o.size().width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(o.size().height, 600.0F, 1e-4F);
    AURORA_TEST_REQUIRE_FALSE(o.child_nodes().empty());
    const Rect &child_bounds = o.child_nodes()[0].bounds();
    AURORA_TEST_CHECK_NEAR(child_bounds.origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(child_bounds.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(child_bounds.size.width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(child_bounds.size.height, 600.0F, 1e-4F);

    // 无子节点：同样占满约束（on_layout 不依赖子节点）。
    PerfOverlay empty;
    LayoutEngine::layout(empty, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(empty.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(empty.size().height, 200.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_perf_overlay
