/// 测试类型: unit
/// 目标单元: include/aurora/perf/trace_writer.h
/// 测试说明: 覆盖 TraceWriter 单例——录制开关门控、Complete/Instant 事件采集、Profiler 帧样本
/// 转换（capture_frame）与计数器快照（capture_counters）、容量溢出丢弃、clear 语义、
/// Chrome Trace Event JSON 格式（元数据头/相位/计数器轨道）、write_json 落盘与错误路径、
/// FrameScope 自动喂数据行为随 AURORA_ENABLE_TRACING 的运行时探测（测试 TU 不写 #if 门控）。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "aurora/core/error_codes.h"
#include "aurora/perf/trace_writer.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_trace_writer {

namespace {

/// @brief 把 TraceWriter 单例恢复到默认配置：停止录制、恢复默认容量并清空数据。
auto reset_trace_writer_to_defaults() -> void {
    TraceWriter& tw = TraceWriter::instance();
    tw.end_capture();
    tw.set_capacity(TraceWriter::AURORA_DEFAULT_CAPACITY, TraceWriter::AURORA_DEFAULT_COUNTER_CAPACITY);
}

/// @brief 用例专属临时文件路径（框架的 per-case 临时目录，用例结束自动清理）。
auto temp_trace_path(const char* name) -> std::string { return aurora::testing::isolation::temp_dir() + "/" + name; }

}  // namespace

AURORA_TEST_CASE(capture_gate_requires_recording) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    // 未录制时采集接口是 no-op。
    AURORA_TEST_CHECK_FALSE(tw.capturing());
    tw.add_complete_event("cold", 1.0, 1.0, 0, 0);
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{0});

    // 录制中：事件入列；end_capture 后停止接收。
    tw.begin_capture();
    AURORA_TEST_CHECK_TRUE(tw.capturing());
    tw.add_complete_event("hot", 1.0, 1.0, 0, 0);
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{1});
    tw.end_capture();
    AURORA_TEST_CHECK_FALSE(tw.capturing());
    tw.add_complete_event("cold_again", 2.0, 1.0, 0, 0);
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{1});
}

AURORA_TEST_CASE(complete_and_instant_events_are_recorded) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    tw.begin_capture();
    tw.add_complete_event("op", 1.0, 2.5, 3, 7);
    tw.add_instant_event("mark", 4.0, 7);
    tw.end_capture();

    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{2});
    const std::vector<TraceEvent>& events = tw.events();
    AURORA_TEST_CHECK_STREQ(events[0].name, "op");
    AURORA_TEST_CHECK_EQ(events[0].phase, TracePhase::Complete);
    AURORA_TEST_CHECK_NEAR(events[0].dur_ms, 2.5, 1e-9);
    AURORA_TEST_CHECK_EQ(events[0].depth, std::uint16_t{3});
    AURORA_TEST_CHECK_EQ(events[0].frame_index, std::uint64_t{7});
    AURORA_TEST_CHECK_STREQ(events[1].name, "mark");
    AURORA_TEST_CHECK_EQ(events[1].phase, TracePhase::Instant);
    AURORA_TEST_CHECK_NEAR(events[1].dur_ms, 0.0, 1e-9);
    AURORA_TEST_CHECK_EQ(tw.dropped_events(), std::uint64_t{0});
}

AURORA_TEST_CASE(to_json_produces_trace_event_array) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    // 空录制也输出合法数组 + 进程/线程元数据事件。
    const std::string empty_json = tw.to_json();
    AURORA_TEST_CHECK_THAT(empty_json, ::aurora::testing::matchers::starts_with("[\n"));
    AURORA_TEST_CHECK_THAT(empty_json, ::aurora::testing::matchers::ends_with("\n]\n"));
    AURORA_TEST_CHECK_THAT(empty_json, ::aurora::testing::matchers::has_substr(R"("name":"process_name")"));
    AURORA_TEST_CHECK_THAT(empty_json, ::aurora::testing::matchers::has_substr(R"("name":"thread_name")"));

    // 录制 Complete + Instant 后：相位字段、分类与秒→微秒换算（ts = 1ms → 1000us）出现。
    tw.begin_capture();
    tw.add_complete_event("op", 1.0, 2.0, 0, 1);
    tw.add_instant_event("mark", 3.0, 1);
    tw.end_capture();

    const std::string json = tw.to_json();
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("ph":"X")"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("ph":"i")"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("cat":"aurora")"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr("1000.000"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("name":"op")"));
}

AURORA_TEST_CASE(capture_frame_copies_profiler_zones_and_counters) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();
    Profiler& prof = Profiler::instance();
    prof.reset();
    RenderCounters::current().reset();

    // 采一帧（一个 zone），帧闭后在录制中抓取：zone → Complete 事件，计数 → 计数器采样。
    prof.begin_frame();
    prof.begin_zone("z");
    prof.end_zone();
    prof.end_frame();

    tw.begin_capture();
    tw.capture_frame(prof);
    RenderCounters snapshot{};
    snapshot.draw_calls = 4;
    tw.capture_counters(prof.frame_index(), Stopwatch::now_ms(), snapshot);
    tw.end_capture();

    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(tw.events()[0].name, "z");
    AURORA_TEST_CHECK_EQ(tw.events()[0].phase, TracePhase::Complete);
    AURORA_TEST_CHECK_EQ(tw.counter_sample_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(tw.counter_samples()[0].frame_index, prof.frame_index());
    AURORA_TEST_CHECK_EQ(tw.counter_samples()[0].counters.draw_calls, std::uint32_t{4});

    // 计数器轨道以 ph:"C" 写出。
    const std::string json = tw.to_json();
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("name":"RenderCounters")"));
    AURORA_TEST_CHECK_THAT(json, ::aurora::testing::matchers::has_substr(R"("ph":"C")"));

    // 未录制时 capture 系列同样是 no-op。
    reset_trace_writer_to_defaults();
    tw.capture_frame(prof);
    tw.capture_counters(1, 1.0, snapshot);
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(tw.counter_sample_count(), std::size_t{0});
}

AURORA_TEST_CASE(capacity_overflow_drops_events_with_counter) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    // 事件与计数器容量各 1：超出的部分丢弃并累加 dropped_events。
    tw.set_capacity(1, 1);
    tw.begin_capture();
    tw.add_complete_event("a", 1.0, 1.0, 0, 0);
    tw.add_complete_event("b", 2.0, 1.0, 0, 0);
    tw.add_instant_event("c", 3.0, 0);
    tw.capture_counters(0, 1.0, RenderCounters{});
    tw.capture_counters(1, 2.0, RenderCounters{});
    tw.end_capture();

    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(tw.counter_sample_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(tw.dropped_events(), std::uint64_t{3});
}

AURORA_TEST_CASE(clear_resets_data_but_keeps_recording_state) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    // clear：清空已录数据与丢弃计数，保留容量配置与录制状态。
    tw.set_capacity(1, 1);
    tw.begin_capture();
    tw.add_complete_event("a", 1.0, 1.0, 0, 0);
    tw.add_complete_event("b", 2.0, 1.0, 0, 0);  // 容量 1：b 被丢弃
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(tw.dropped_events(), std::uint64_t{1});

    tw.clear();
    AURORA_TEST_CHECK_EQ(tw.event_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(tw.dropped_events(), std::uint64_t{0});
    AURORA_TEST_CHECK_TRUE(tw.capturing());  // 录制状态保留
}

AURORA_TEST_CASE(write_json_writes_readable_file) {
    reset_trace_writer_to_defaults();
    TraceWriter& tw = TraceWriter::instance();

    tw.begin_capture();
    tw.add_complete_event("filed", 1.0, 2.0, 0, 1);
    tw.end_capture();

    const std::string path = temp_trace_path("utest_trace_writer.trace.json");
    const Result<bool> written = tw.write_json(path.c_str());
    AURORA_TEST_REQUIRE_TRUE(written.ok());
    AURORA_TEST_CHECK_TRUE(written.value());

    // 读回：文件非空且是 Trace Event JSON（含元数据头与本事件名）。
    std::ifstream in{path, std::ios::binary};
    AURORA_TEST_REQUIRE_TRUE(in.is_open());
    const std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    AURORA_TEST_CHECK_FALSE(content.empty());
    AURORA_TEST_CHECK_THAT(content, ::aurora::testing::matchers::has_substr(R"("name":"process_name")"));
    AURORA_TEST_CHECK_THAT(content, ::aurora::testing::matchers::has_substr(R"("name":"filed")"));
}

AURORA_TEST_CASE(write_json_rejects_empty_path) {
    reset_trace_writer_to_defaults();
    const TraceWriter& tw = TraceWriter::instance();

    // 空路径：结构化错误（IOFileNotFound），不抛异常。
    const Result<bool> refused = tw.write_json("");
    AURORA_TEST_REQUIRE_FALSE(refused.ok());  // 致命：失败时才可安全解引用 Error
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::IOFileNotFound);
    AURORA_TEST_CHECK_FALSE(refused.error().message.empty());
}

AURORA_TEST_CASE(frame_scope_auto_feed_matches_tracing_flag) {
    // 运行时探测 AURORA_ENABLE_TRACING（测试 TU 不写 #if 门控）：
    // 开启时 FrameScope 析构自动喂数据（计数器采样 +1）；关闭时不喂数据。
    reset_trace_writer_to_defaults();
    Profiler::instance().reset();
    RenderCounters::current().reset();
    TraceWriter& tw = TraceWriter::instance();

    tw.begin_capture();
    const std::size_t before_samples = tw.counter_sample_count();
    const std::size_t before_events = tw.event_count();
    {
        aurora::FrameScope frame;
        (void)frame;
    }
    const bool fed = tw.counter_sample_count() == before_samples + 1;
    AURORA_TEST_CHECK_EQ(fed, aurora::tracing_enabled());

    if (aurora::tracing_enabled()) {
        // 开启态：计数器采样自动入列，事件列表不减少。
        AURORA_TEST_CHECK_GE(tw.counter_sample_count(), before_samples + 1);
        AURORA_TEST_CHECK_GE(tw.event_count(), before_events);
    } else {
        // 关闭态：FrameScope 完全不触碰录制器。
        AURORA_TEST_CHECK_EQ(tw.counter_sample_count(), before_samples);
        AURORA_TEST_CHECK_EQ(tw.event_count(), before_events);
    }
    tw.end_capture();
}

}  // namespace aurora::test_cases::utest_trace_writer
