/// 测试类型: unit
/// 目标单元: include/aurora/perf/perf_session.h
/// 测试说明: 覆盖 PerfSession/PerfReport——构造默认值、逐帧记录与百分位/抖动/超预算统计、
/// 计数器累加与峰值（sum/max）、full_redraw 帧计数、空会话安全回退、begin 复用、report 幂等、
/// 显式计数快照与全局单例两种数据源、zone 跨帧合并行为随 AURORA_ENABLE_PROFILING 的运行时探测、
/// JSON/Markdown/CSV 三种报告形态的基本格式。时间值全部由测试注入，不依赖真实计时。

#include <algorithm>
#include <cstdint>
#include <string>

#include "aurora/perf/perf_session.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_perf_session {

namespace {

/// @brief 统计字符出现次数（CSV 列数 = 逗号数 + 1）。
auto count_of(const std::string &text, char ch) -> std::size_t {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), ch));
}

}  // namespace

AURORA_TEST_CASE(constructor_defaults_and_name) {
    // 默认会话名 "session"、默认 60fps 帧预算、零样本。
    const PerfSession defaults{};
    AURORA_TEST_CHECK_STREQ(defaults.name().c_str(), "session");
    AURORA_TEST_CHECK_NEAR(defaults.frame_budget_ms(), 16.67, 1e-9);
    AURORA_TEST_CHECK_EQ(defaults.frame_count(), std::size_t{0});

    // 自定义会话名与预留容量（预留量不影响可观测行为）。
    const PerfSession named{"bench-300", 8};
    AURORA_TEST_CHECK_STREQ(named.name().c_str(), "bench-300");
    AURORA_TEST_CHECK_EQ(named.frame_count(), std::size_t{0});
}

AURORA_TEST_CASE(record_and_report_frame_statistics) {
    // 注入 3 帧 {10, 20, 30} ms，验证分布统计（线性插值百分位、总体标准差）。
    PerfSession sess{"stats", 8};
    const RenderCounters none{};
    sess.record_frame(10.0, none);
    sess.record_frame(20.0, none);
    sess.record_frame(30.0, none);

    AURORA_TEST_CHECK_EQ(sess.frame_count(), std::size_t{3});
    const PerfReport r = sess.report();
    AURORA_TEST_CHECK_STREQ(r.name.c_str(), "stats");
    AURORA_TEST_CHECK_NEAR(r.total_ms, 60.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.avg_frame_ms, 20.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.best_ms, 10.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.worst_ms, 30.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.p50_ms, 20.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.p95_ms, 29.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.p99_ms, 29.8, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.jitter_ms, 8.16497, 1e-4);

    // 默认预算 16.67ms：20/30 两帧超预算。
    AURORA_TEST_CHECK_EQ(r.over_budget_frames, std::size_t{2});
    AURORA_TEST_CHECK_NEAR(r.over_budget_ratio(), 2.0 / 3.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.frame_budget_ms, 16.67, 1e-9);

    // 逐帧耗时按记录顺序保留。
    const std::vector<double> &times = sess.frame_times_ms();
    AURORA_TEST_REQUIRE_THAT(times, ::aurora::testing::matchers::size_is(3));
    AURORA_TEST_CHECK_NEAR(times[0], 10.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(times[1], 20.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(times[2], 30.0, 1e-9);
}

AURORA_TEST_CASE(frame_budget_controls_over_budget_count) {
    // 帧预算可配置：25ms 预算下仅 30ms 一帧超预算。
    PerfSession sess{"budget", 8};
    const RenderCounters none{};
    sess.record_frame(10.0, none);
    sess.record_frame(20.0, none);
    sess.record_frame(30.0, none);
    sess.set_frame_budget_ms(25.0);

    AURORA_TEST_CHECK_NEAR(sess.frame_budget_ms(), 25.0, 1e-9);
    const PerfReport r = sess.report();
    AURORA_TEST_CHECK_EQ(r.over_budget_frames, std::size_t{1});
    AURORA_TEST_CHECK_NEAR(r.over_budget_ratio(), 1.0 / 3.0, 1e-9);
}

AURORA_TEST_CASE(counters_sum_max_and_full_redraw_frames) {
    // 显式计数快照：sum 逐字段累加、max 逐字段取峰值、full_redraw 计超整帧重绘的帧数。
    PerfSession sess{"counters", 8};

    RenderCounters first{};
    first.fill_rects = 5;
    first.dirty_area_ratio = 0.1;
    first.full_redraw = false;
    sess.record_frame(5.0, first);

    RenderCounters second{};
    second.fill_rects = 3;
    second.dirty_area_ratio = 0.3;
    second.full_redraw = true;
    sess.record_frame(5.0, second);

    const PerfReport r = sess.report();
    AURORA_TEST_CHECK_EQ(r.counters_sum.fill_rects, std::uint32_t{8});
    AURORA_TEST_CHECK_EQ(r.counters_max.fill_rects, std::uint32_t{5});
    AURORA_TEST_CHECK_NEAR(r.counters_sum.dirty_area_ratio, 0.4, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.avg_dirty_area_ratio(), 0.2, 1e-9);
    AURORA_TEST_CHECK_EQ(r.full_redraw_frames, std::size_t{1});
}

AURORA_TEST_CASE(empty_session_report_is_safe) {
    // 空会话：所有派生指标安全回退 0，不产生 NaN/除零；序列化仍非空。
    const PerfSession sess{"empty", 4};
    AURORA_TEST_CHECK_EQ(sess.frame_count(), std::size_t{0});

    const PerfReport r = sess.report();
    AURORA_TEST_CHECK_EQ(r.frame_count, std::size_t{0});
    AURORA_TEST_CHECK_NEAR(r.avg_frame_ms, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.p99_ms, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.jitter_ms, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.over_budget_ratio(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(r.avg_dirty_area_ratio(), 0.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(r.zones.empty());

    AURORA_TEST_CHECK_FALSE(r.to_json().empty());
    AURORA_TEST_CHECK_FALSE(r.to_markdown().empty());
    AURORA_TEST_CHECK_FALSE(r.to_csv_row().empty());
}

AURORA_TEST_CASE(begin_reuses_session_and_keeps_configuration) {
    // begin 清空采样数据以复用会话；会话名与帧预算保留。
    PerfSession sess{"reused", 4};
    const RenderCounters none{};
    sess.record_frame(10.0, none);
    sess.record_frame(20.0, none);
    sess.set_frame_budget_ms(30.0);

    sess.begin();
    AURORA_TEST_CHECK_EQ(sess.frame_count(), std::size_t{0});
    AURORA_TEST_CHECK_STREQ(sess.name().c_str(), "reused");
    AURORA_TEST_CHECK_NEAR(sess.frame_budget_ms(), 30.0, 1e-9);

    sess.record_frame(5.0, none);
    AURORA_TEST_CHECK_EQ(sess.report().frame_count, std::size_t{1});
    AURORA_TEST_CHECK_NEAR(sess.report().avg_frame_ms, 5.0, 1e-9);
}

AURORA_TEST_CASE(report_is_repeatable_and_non_mutating) {
    // report 不修改会话状态：连续两次结算结果一致。
    PerfSession sess{"idempotent", 8};
    const RenderCounters none{};
    sess.record_frame(12.0, none);
    sess.record_frame(18.0, none);

    const PerfReport first = sess.report();
    const PerfReport second = sess.report();
    AURORA_TEST_CHECK_EQ(first.frame_count, second.frame_count);
    AURORA_TEST_CHECK_NEAR(first.avg_frame_ms, second.avg_frame_ms, 1e-12);
    AURORA_TEST_CHECK_NEAR(first.p99_ms, second.p99_ms, 1e-12);
    AURORA_TEST_CHECK_NEAR(first.jitter_ms, second.jitter_ms, 1e-12);
    AURORA_TEST_CHECK_EQ(sess.frame_count(), std::size_t{2});  // 会话状态未被报告消耗
}

AURORA_TEST_CASE(record_frame_reads_global_counter_singleton) {
    // 单参重载从进程级 RenderCounters::current() 取数（与显式重载互补）。
    PerfSession sess{"singleton", 4};
    RenderCounters::current().reset();
    RenderCounters::current().fill_rects = 7;
    RenderCounters::current().full_redraw = true;

    sess.record_frame(5.0);
    AURORA_TEST_CHECK_EQ(sess.report().counters_sum.fill_rects, std::uint32_t{7});
    AURORA_TEST_CHECK_EQ(sess.report().full_redraw_frames, std::size_t{1});

    RenderCounters::current().reset();  // 恢复：不把脏状态泄漏给后续用例
}

AURORA_TEST_CASE(zone_merge_matches_profiling_flag) {
    // zone 是否并入会话统计与编译期插桩开关一致（运行时探测，不写 #if 门控）。
    Profiler &prof = Profiler::instance();
    prof.reset();
    prof.set_enabled(true);

    PerfSession sess{"zones", 8};
    RenderCounters::current().reset();

    // 两帧各采一个同名 zone：开启时跨帧按名字合并（call_count == 2）。
    for (int i = 0; i < 2; ++i) {
        prof.begin_frame();
        prof.begin_zone("frame_zone");
        prof.end_zone();
        prof.end_frame();
        sess.record_frame(1.0);
    }

    const PerfReport r = sess.report();
    if (aurora::profiling_enabled()) {
        AURORA_TEST_REQUIRE_EQ(r.zones.size(), std::size_t{1});
        AURORA_TEST_CHECK_STREQ(r.zones[0].name, "frame_zone");
        AURORA_TEST_CHECK_EQ(r.zones[0].call_count, std::uint32_t{2});
        AURORA_TEST_CHECK_EQ(r.long_task_count, std::size_t{0});
    } else {
        // 关闭态：record_frame 不读 Profiler，zone 与长任务恒不计入。
        AURORA_TEST_CHECK_TRUE(r.zones.empty());
        AURORA_TEST_CHECK_EQ(r.long_task_count, std::size_t{0});
    }
    prof.reset();
}

AURORA_TEST_CASE(report_serialization_shapes) {
    // JSON / Markdown / CSV 三种形态的基本结构；CSV 行列数与表头严格对应。
    PerfSession sess{"sess", 8};
    const RenderCounters none{};
    sess.record_frame(8.0, none);
    sess.record_frame(24.0, none);
    const PerfReport r = sess.report();

    const std::string json = r.to_json();
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("name":"sess")"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("frame_count":2)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("counters_sum":)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("counters_max":)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("zones":[)"));

    const std::string markdown = r.to_markdown();
    AURORA_TEST_CHECK_THAT(markdown, ::aurora::testing::matchers::has_substr("sess"));
    AURORA_TEST_CHECK_THAT(markdown, ::aurora::testing::matchers::has_substr("p99"));

    const std::string header = PerfReport::csv_header();
    const std::string row = r.to_csv_row();
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::starts_with("name,"));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::ends_with("scroll_buffer_bytes"));
    AURORA_TEST_CHECK_THAT(row, ::aurora::testing::matchers::starts_with("sess,"));
    AURORA_TEST_CHECK_EQ(count_of(header, ','), count_of(row, ','));
}

}  // namespace aurora::test_cases::utest_perf_session
