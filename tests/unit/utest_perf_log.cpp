/// 测试类型: unit
/// 目标单元: include/aurora/perf/perf_log.h
/// 测试说明: 覆盖 PerfLog——enable/disable/enabled 开关语义、on_frame_end 的周期输出路径
/// （禁用时 no-op、启用时按 interval 触发）、snapshot_json / snapshot_csv 与 FrameStats 及
/// RenderCounters 的同源一致性、profiling 标记随 AURORA_ENABLE_PROFILING 的运行时探测
/// （测试 TU 不写 #if 门控）、csv_header 与 RenderCounters::csv_header 的拼接契约。
/// 不直接调用 dump_json/dump_csv（AURORA_LOG_RAW 通道输出，避免污染 runner stdout），
/// 其内容与 snapshot_json/snapshot_csv 一致由同源实现保证。

#include <string>

#include "aurora/app/perf_overlay.h"  // FrameStats锛歅erfLog 鐨勫抚缁熻鏁版嵁婧愶紙濮嬬粓鍙敤锛?#10;#include "aurora/perf/counters.h"
#include "aurora/perf/perf_log.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_perf_log {

namespace {

/// @brief 准备确定性的 FrameStats（两帧 10ms/20ms → fps 66.7、avg 15.0）与全零计数器单例。
auto seed_framestats_and_counters() -> void {
    FrameStats::instance().reset();
    FrameStats::instance().record(0.010);
    FrameStats::instance().record(0.020);
    RenderCounters::current().reset();
}

}  // namespace

AURORA_TEST_CASE(enable_disable_roundtrip) {
    // 开关语义：disable 兜底后翻转 enable/disable 状态可读回。
    PerfLog::disable();
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());

    PerfLog::enable(2);
    AURORA_TEST_CHECK_TRUE(PerfLog::enabled());

    PerfLog::disable();
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());
}

AURORA_TEST_CASE(on_frame_end_is_noop_when_disabled) {
    // 禁用状态：on_frame_end 是 no-op（不计数、不输出、不改变启用态）。
    PerfLog::disable();
    AURORA_TEST_CHECK_NO_THROW(PerfLog::on_frame_end());
    AURORA_TEST_CHECK_NO_THROW(PerfLog::on_frame_end());
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());
}

AURORA_TEST_CASE(on_frame_end_triggers_summary_after_interval) {
    // 启用状态：周期计数达到 interval 后触发一次摘要输出（诊断通道，走 stderr），
    // 执行路径本身必须无异常；interval 重设由 enable 重置计数保证可重复触发。
    PerfLog::enable(2);
    AURORA_TEST_CHECK_NO_THROW(PerfLog::on_frame_end());  // 计数 1：未到周期
    AURORA_TEST_CHECK_NO_THROW(PerfLog::on_frame_end());  // 计数 2：触发一次摘要
    AURORA_TEST_CHECK_NO_THROW(PerfLog::on_frame_end());  // 计数重置后再累计
    PerfLog::disable();
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());
}

AURORA_TEST_CASE(snapshot_json_reflects_framestats_and_counters) {
    seed_framestats_and_counters();

    const std::string json = PerfLog::snapshot_json();

    // FrameStats 部分：两帧 10ms/20ms → fps 66.7、avg 15.0（%.1f 格式）。
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("fps":66.7)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("avg_ms":15.0)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("p99_ms":)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("total_frames":2)"));

    // counters 子对象与进程级单例同源；profiling 标记与编译期插桩开关一致（运行时探测）。
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("counters":)"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(RenderCounters::current().to_json()));
    const bool json_says_profiling = json.find(R"("profiling":true)") != std::string::npos;
    AURORA_TEST_CHECK_EQ(json_says_profiling, aurora::profiling_enabled());

    FrameStats::instance().reset();
}

AURORA_TEST_CASE(snapshot_csv_has_header_comment_and_data_row) {
    seed_framestats_and_counters();

    const std::string csv = PerfLog::snapshot_csv();

    // 结构：注释行 "# <header>" + 单数据行；数据行尾部与计数器单例的 CSV 行拼接。
    AURORA_TEST_CHECK_THAT(csv, ::aurora::testing::matchers::starts_with("# "));
    AURORA_TEST_CHECK_THAT(csv, ::aurora::testing::matchers::has_substr(PerfLog::csv_header()));
    // 恰好两行：换行仅在注释行尾；数据行不带尾换行（与 write 风格一致，拼接外部行更友好）。
    const auto first_nl = csv.find('\n');
    AURORA_TEST_CHECK(first_nl != std::string::npos);
    AURORA_TEST_CHECK_EQ(csv.find('\n', first_nl + 1), std::string::npos);
    AURORA_TEST_CHECK_THAT(csv, ::aurora::testing::matchers::ends_with(RenderCounters::current().to_csv_row()));

    // 全零计数器：dirty_area_ratio 四位小数、full_redraw 与 scroll_buffer_bytes 为 0。
    AURORA_TEST_CHECK_THAT(csv, ::aurora::testing::matchers::has_substr(",0.0000,0,0"));

    FrameStats::instance().reset();
}

AURORA_TEST_CASE(csv_header_appends_rendercounters_columns) {
    // 表头 = FrameStats 指标列 + RenderCounters 全部列（与 snapshot_csv 数据行严格对应）。
    const std::string header = PerfLog::csv_header();
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::starts_with("fps,avg_ms,p99_ms,jitter_ms"));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::ends_with(RenderCounters::csv_header()));
    // 中段含分阶段计时与 profiling 标记列。
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::has_substr("layout_ms,paint_ms,present_ms"));
    AURORA_TEST_CHECK_THAT(header, ::aurora::testing::matchers::has_substr("profiling"));
}

}  // namespace aurora::test_cases::utest_perf_log
