// test_perf_trace_format.cpp — `aurora::TraceWriter` 输出格式与录制语义单测。
//
// trace 文件是给 chrome://tracing / Perfetto 吃的，格式错了不会报错，只会「打开是空的」，
// 因此这里用 nlohmann::json **真解析**产物并逐字段校验 Trace Event 规范要点：
//   - 顶层是数组；
//   - 元数据事件 ph=="M"；zone 事件 ph=="X" 且带 ts/dur；瞬时事件 ph=="i"；
//   - 计数器事件 ph=="C"；
//   - 时间单位是**微秒**（内部存 ms，导出须 ×1000）。
// 另外覆盖有界内存语义（超容量丢弃并计数）与「未录制时不接收事件」。
//
// `AURORA_ENABLE_TRACING` 关闭时 `TraceWriter` 依然可被显式驱动（只是 `FrameScope`
// 不再自动喂数据），故本文件的断言在两种构建下一致；仅「自动采集」那一条按开关分流。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::ErrorCode;
using aurora::FrameScope;
using aurora::Profiler;
using aurora::RenderCounters;
using aurora::Result;
using aurora::Stopwatch;
using aurora::TraceEvent;
using aurora::TracePhase;
using aurora::TraceWriter;
using aurora::tracing_enabled;

using Json = nlohmann::json;

namespace {

/// @brief 复位录制器到干净状态。
auto fresh() -> TraceWriter & {
    TraceWriter &tw = TraceWriter::instance();
    tw.end_capture();
    tw.set_capacity(TraceWriter::AURORA_DEFAULT_CAPACITY, TraceWriter::AURORA_DEFAULT_COUNTER_CAPACITY);
    return tw;
}

/// @brief 解析 trace JSON；失败返回 discarded。
[[nodiscard]] auto parse(const std::string &s) -> Json { return Json::parse(s, nullptr, false); }

/// @brief 在事件数组中找到首个满足 `name` 与 `ph` 的元素下标；找不到返回 npos。
[[nodiscard]] auto find_event(const Json &arr, const char *name, const char *ph) -> std::size_t {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const Json &e = arr[i];
        if (e.value("name", std::string{}) == name && e.value("ph", std::string{}) == ph) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

// ---- Test 1: 录制开关语义 ----
auto test_capture_switch() -> void {
    TraceWriter &tw = fresh();

    AURORA_TEST_CHECK_MSG(!tw.capturing(), "Test1: not capturing initially");

    tw.add_complete_event("ignored", 1.0, 1.0, 0, 0);
    AURORA_TEST_CHECK_MSG(tw.event_count() == 0, "Test1: no events received when not capturing");
    AURORA_TEST_CHECK_MSG(tw.dropped_events() == 0,
                          "Test1: not counted as dropped when not capturing (it is 'off', not 'lost')");

    tw.begin_capture();
    AURORA_TEST_CHECK_MSG(tw.capturing(), "Test1: capturing after begin_capture");
    tw.add_complete_event("kept", 1.0, 2.0, 0, 0);
    AURORA_TEST_CHECK_MSG(tw.event_count() == 1, "Test1: events received while capturing");

    tw.end_capture();
    AURORA_TEST_CHECK_MSG(!tw.capturing(), "Test1: capture stops after end_capture");
    AURORA_TEST_CHECK_MSG(tw.event_count() == 1, "Test1: end_capture preserves recorded data for export");

    tw.add_complete_event("after", 1.0, 1.0, 0, 0);
    AURORA_TEST_CHECK_MSG(tw.event_count() == 1, "Test1: no new events received after stopping");

    tw.begin_capture();
    AURORA_TEST_CHECK_MSG(tw.event_count() == 0, "Test1: begin_capture again clears existing events");
    tw.end_capture();
}

// ---- Test 2: 有界内存——超容量丢弃并计数，绝不无限增长 ----
auto test_bounded_memory() -> void {
    TraceWriter &tw = fresh();
    tw.set_capacity(3, 2);
    AURORA_TEST_CHECK_MSG(tw.capacity() == 3, "Test2: set_capacity takes effect");

    tw.begin_capture();
    for (int i = 0; i < 10; ++i) {
        tw.add_complete_event("z", i, 1.0, 0, 0);
    }
    AURORA_TEST_CHECK_MSG(tw.event_count() == 3, "Test2: event count truncated to 3 by capacity");
    AURORA_TEST_CHECK_MSG(tw.dropped_events() == 7, "Test2: dropped_events() == 7");

    for (int i = 0; i < 5; ++i) {
        tw.capture_counters(static_cast<std::uint64_t>(i), 1.0, RenderCounters{});
    }
    AURORA_TEST_CHECK_MSG(tw.counter_sample_count() == 2, "Test2: counter samples truncated to 2 by capacity");
    AURORA_TEST_CHECK_MSG(tw.dropped_events() == 7 + 3, "Test2: counter overflow also counted as dropped");

    tw.end_capture();
    tw.clear();
    AURORA_TEST_CHECK_MSG(tw.event_count() == 0 && tw.dropped_events() == 0,
                          "Test2: clear wipes data and dropped count");
    AURORA_TEST_CHECK_MSG(tw.capacity() == 3, "Test2: clear preserves capacity config");

    fresh();
}

// ---- Test 3: to_json 结构合法 + 元数据事件 ----
auto test_json_structure() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();
    tw.add_complete_event("paint", 10.0, 2.5, 1, 42);
    tw.add_instant_event("frame-start", 10.0, 42);
    tw.end_capture();

    const Json j = parse(tw.to_json());
    AURORA_TEST_CHECK_MSG(!j.is_discarded(), "Test3: to_json output is parseable");
    if (j.is_discarded()) {
        return;
    }

    AURORA_TEST_CHECK_MSG(j.is_array(), "Test3: top level is a JSON array (Trace Event array format)");
    // 2 条元数据 + 2 条事件
    AURORA_TEST_CHECK_MSG(j.size() == 4, "Test3: total event count == 2 metadata + 2 events");

    const std::size_t pn = find_event(j, "process_name", "M");
    const std::size_t tn = find_event(j, "thread_name", "M");
    AURORA_TEST_CHECK_MSG(std::cmp_not_equal(pn, -1), "Test3: contains process_name metadata event");
    AURORA_TEST_CHECK_MSG(std::cmp_not_equal(tn, -1), "Test3: contains thread_name metadata event");
    if (std::cmp_not_equal(pn, -1)) {
        AURORA_TEST_CHECK_MSG(j[pn]["args"].value("name", std::string{}) == "aurora", "Test3: process name is aurora");
    }
}

// ---- Test 4: Complete 事件字段与微秒换算 ----
auto test_complete_event_fields() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();
    tw.add_complete_event("paint", 10.0, 2.5, 3, 42);
    tw.end_capture();

    const Json j = parse(tw.to_json());
    if (j.is_discarded()) {
        return;
    }

    const std::size_t idx = find_event(j, "paint", "X");
    AURORA_TEST_CHECK_MSG(std::cmp_not_equal(idx, -1), "Test4: found ph==\"X\" paint event");
    if (std::cmp_equal(idx, -1)) { // NOLINT
        return;
    }

    const Json &e = j[idx];
    AURORA_TEST_CHECK_MSG(e.value("cat", std::string{}) == "aurora", "Test4: cat == aurora");
    AURORA_TEST_CHECK_MSG(near_d(e.value("ts", 0.0), 10000.0, 1e-3),
                          "Test4: ts written in microseconds (10ms → 10000us)");
    AURORA_TEST_CHECK_MSG(near_d(e.value("dur", 0.0), 2500.0, 1e-3),
                          "Test4: dur written in microseconds (2.5ms → 2500us)");
    AURORA_TEST_CHECK_MSG(e.contains("pid") && e.contains("tid"), "Test4: contains pid / tid");
    AURORA_TEST_CHECK_MSG(e["args"].value("frame", 0ull) == 42ull, "Test4: args.frame == 42");
    AURORA_TEST_CHECK_MSG(e["args"].value("depth", 0u) == 3u, "Test4: args.depth == 3");
    AURORA_TEST_CHECK_MSG(e["args"].contains("long_task"), "Test4: contains args.long_task attribution field");
}

// ---- Test 5: Instant 事件字段 ----
auto test_instant_event_fields() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();
    tw.add_instant_event("frame-start", 7.0, 9);
    tw.end_capture();

    const Json j = parse(tw.to_json());
    if (j.is_discarded()) {
        return;
    }

    const std::size_t idx = find_event(j, "frame-start", "i");
    AURORA_TEST_CHECK_MSG(std::cmp_not_equal(idx, -1), "Test5: found ph==\"i\" instant event");
    if (std::cmp_equal(idx, -1)) { // NOLINT
        return;
    }

    const Json &e = j[idx];
    AURORA_TEST_CHECK_MSG(near_d(e.value("ts", 0.0), 7000.0, 1e-3), "Test5: ts written in microseconds");
    AURORA_TEST_CHECK_MSG(!e.contains("dur"), "Test5: instant event does not write dur");
    AURORA_TEST_CHECK_MSG(e.value("s", std::string{}) == "t", "Test5: scope s == \"t\" (thread level)");
}

// ---- Test 6: 计数器轨道事件 ph == "C" ----
auto test_counter_track() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();

    RenderCounters c{};
    c.draw_calls = 11;
    c.layout_nodes = 22;
    c.paint_nodes = 33;
    c.dirty_area_ratio = 0.5;
    c.full_redraw = true;
    tw.capture_counters(5, 3.0, c);

    tw.end_capture();
    AURORA_TEST_CHECK_MSG(tw.counter_sample_count() == 1, "Test6: records 1 counter sample");

    const Json j = parse(tw.to_json());
    if (j.is_discarded()) {
        return;
    }

    const std::size_t idx = find_event(j, "RenderCounters", "C");
    AURORA_TEST_CHECK_MSG(std::cmp_not_equal(idx, -1), "Test6: found ph==\"C\" counter event");
    if (std::cmp_equal(idx, -1)) { // NOLINT
        return;
    }

    const Json &args = j[idx]["args"];
    AURORA_TEST_CHECK_MSG(near_d(j[idx].value("ts", 0.0), 3000.0, 1e-3),
                          "Test6: counter event ts written in microseconds");
    AURORA_TEST_CHECK_MSG(args.value("frame", 0ull) == 5ull, "Test6: args.frame == 5");
    AURORA_TEST_CHECK_MSG(args.value("draw_calls", 0u) == 11u, "Test6: args.draw_calls == 11");
    AURORA_TEST_CHECK_MSG(args.value("layout_nodes", 0u) == 22u, "Test6: args.layout_nodes == 22");
    AURORA_TEST_CHECK_MSG(args.value("paint_nodes", 0u) == 33u, "Test6: args.paint_nodes == 33");
    AURORA_TEST_CHECK_MSG(args.value("full_redraw", 0) == 1,
                          "Test6: full_redraw written as 0/1 (Perfetto counter track needs numeric)");
}

// ---- Test 7: capture_frame 从 Profiler 转录（含长任务标记）----
auto test_capture_frame() -> void {
    TraceWriter &tw = fresh();
    Profiler &p = Profiler::instance();
    p.set_enabled(true);
    p.set_zone_capacity(Profiler::AURORA_DEFAULT_ZONE_CAPACITY);
    p.reset();
    p.set_long_task_threshold_ms(2.0);

    p.begin_frame();
    p.begin_zone("slow-zone");
    {
        const Stopwatch sw;
        volatile double sink = 0.0;
        while (sw.elapsed_ms() < 4.0) {
            sink += 1.0;
        }
        (void)sink;
    }
    p.end_zone();
    p.begin_zone("fast-zone");
    p.end_zone();
    p.end_frame();

    tw.begin_capture();
    tw.capture_frame(p); // 须在 end_frame 之后、下一次 begin_frame 之前
    tw.end_capture();

    AURORA_TEST_CHECK_MSG(tw.event_count() == 2, "Test7: transcribes 2 zone events");

    bool slow_marked = false;
    bool fast_unmarked = false;
    for (const TraceEvent &e : tw.events()) {
        if (e.name != nullptr && std::string(e.name) == "slow-zone") {
            slow_marked = e.long_task;
        }
        if (e.name != nullptr && std::string(e.name) == "fast-zone") {
            fast_unmarked = !e.long_task;
        }
        AURORA_TEST_CHECK_MSG(e.phase == TracePhase::Complete, "Test7: zone transcribed as Complete phase");
    }
    AURORA_TEST_CHECK_MSG(slow_marked, "Test7: zone exceeding threshold marked as long_task");
    AURORA_TEST_CHECK_MSG(fast_unmarked, "Test7: zone below threshold not marked");

    p.set_long_task_threshold_ms(Profiler::AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS);
    p.reset();
}

// ---- Test 8: write_json 落盘且内容可解析；非法路径返回错误 ----
auto test_write_json() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();
    tw.add_complete_event("disk", 1.0, 1.0, 0, 0);
    tw.end_capture();

    const auto *path = "test_perf_trace_format.trace.json";
    const Result<bool> wrote = tw.write_json(path);
    AURORA_TEST_CHECK_MSG(wrote.ok() && wrote.value(), "Test8: write_json succeeds");

    std::ifstream f(path, std::ios::binary);
    AURORA_TEST_CHECK_MSG(f.is_open(), "Test8: output file exists");
    if (f.is_open()) {
        std::ostringstream ss;
        ss << f.rdbuf();
        f.close();
        const Json j = parse(ss.str());
        AURORA_TEST_CHECK_MSG(!j.is_discarded() && j.is_array(), "Test8: written content is a valid JSON array");
        AURORA_TEST_CHECK_MSG(find_event(j, "disk", "X") != static_cast<std::size_t>(-1),
                              "Test8: written content contains expected event");
    }
    std::remove(path);

    const Result<bool> bad = tw.write_json("");
    AURORA_TEST_CHECK_MSG(!bad.ok(), "Test8: empty path returns error instead of crashing");
    AURORA_TEST_CHECK_MSG(bad.error().code_enum == ErrorCode::IOFileNotFound, "Test8: error code is IOFileNotFound");
}

// ---- Test 9: 事件名 JSON 转义 ----
auto test_name_escaping() -> void {
    TraceWriter &tw = fresh();
    tw.begin_capture();
    // 静态字符串字面量：生命周期覆盖整个进程，符合 TraceEvent::name 的所有权约定。
    static const auto *tricky = "quote\"back\\slash\ttab";
    tw.add_complete_event(tricky, 1.0, 1.0, 0, 0);
    tw.end_capture();

    const Json j = parse(tw.to_json());
    AURORA_TEST_CHECK_MSG(!j.is_discarded(), "Test9: name with special chars still yields valid JSON");
    if (j.is_discarded()) {
        return;
    }

    bool found = false;
    for (const Json &e : j) {
        if (e.value("name", std::string{}) == tricky) {
            found = true;
        }
    }
    AURORA_TEST_CHECK_MSG(found, "Test9: escaped name parses back to original");
}

// ---- Test 10: tracing_enabled() 与宏定义一致；FrameScope 自动采集按开关分流 ----
auto test_tracing_switch() -> void {
    constexpr bool on = tracing_enabled();
#ifdef AURORA_ENABLE_TRACING
    AURORA_TEST_CHECK_MSG(on, "Test10: tracing_enabled() == true when AURORA_ENABLE_TRACING defined");
#else
    AURORA_TEST_CHECK_MSG(!on, "Test10: tracing_enabled() == false when AURORA_ENABLE_TRACING undefined");
#endif

    TraceWriter &tw = fresh();
    Profiler &p = Profiler::instance();
    p.reset();

    tw.begin_capture();
    {
        // 直接构造 FrameScope（而非用 AURORA_PROFILE_FRAME 宏），使本用例在
        // PROFILING OFF 的构建下同样能驱动到 detail::on_frame_scope_end()。
        const FrameScope fs;
        Profiler::instance().begin_zone("auto");
        Profiler::instance().end_zone();
    }
    tw.end_capture();

    if constexpr (on) { // NOLINT
        AURORA_TEST_CHECK_MSG(tw.event_count() >= 1, "Test10[TRACING=ON]: FrameScope auto-feeds zone event");
        AURORA_TEST_CHECK_MSG(tw.counter_sample_count() == 1,
                              "Test10[TRACING=ON]: FrameScope auto-feeds counter sample");
    } else {
        AURORA_TEST_CHECK_MSG(tw.event_count() == 0, "Test10[TRACING=OFF]: FrameScope does not auto-feed data");
        AURORA_TEST_CHECK_MSG(tw.counter_sample_count() == 0, "Test10[TRACING=OFF]: no counter sample");
    }
    AURORA_TEST_PRINTF("      (build profile: tracing=%s)\n", on ? "ON" : "OFF");

    p.reset();
    RenderCounters::current().reset();
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_perf_trace_format ===\n");

    test_capture_switch();
    test_bounded_memory();
    test_json_structure();
    test_complete_event_fields();
    test_instant_event_fields();
    test_counter_track();
    test_capture_frame();
    test_write_json();
    test_name_escaping();
    test_tracing_switch();

    fresh();
    TraceWriter::instance().clear();
}
