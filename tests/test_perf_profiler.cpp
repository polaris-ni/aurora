// test_perf_profiler.cpp — `aurora::Profiler` / `ScopedTimer` / `FrameScope` 单测。
//
// 关键点：`Profiler` 是**进程级单例**，且其成员函数在 `AURORA_ENABLE_PROFILING`
// 关闭时依然参与编译与链接（只有埋点宏被展开为空）。因此本文件一律**直接调用类接口**
// 而非埋点宏，使断言在 PROFILING ON / OFF 两种构建下都成立；宏是否被裁剪由
// test_perf_counters.cpp 负责。
//
// 每个用例开头 `reset()`，避免用例间通过单例互相污染。
#include <cstdint>
#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::FrameScope;
using aurora::Profiler;
using aurora::RenderCounters;
using aurora::ScopedTimer;
using aurora::Stopwatch;
using aurora::ZoneAggregate;

namespace {

/// @brief 忙等至少 `target_ms` 毫秒（sleep 的调度粒度撑不起「至少」的语义）。
auto busy_wait_ms(double target_ms) -> void {
    const Stopwatch sw;
    volatile double sink = 0.0;
    while (sw.elapsed_ms() < target_ms) {
        sink += 1.0;
    }
    (void)sink;
}

/// @brief 复位单例到干净状态（含容量与阈值——`reset()` 刻意保留这两项配置）。
auto fresh() -> Profiler & {
    Profiler &p = Profiler::instance();
    p.set_enabled(true);
    p.set_zone_capacity(Profiler::AURORA_DEFAULT_ZONE_CAPACITY);
    p.set_long_task_threshold_ms(Profiler::AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS);
    p.reset();
    return p;
}

// ---- Test 1: 帧协议——帧序号推进、begin_frame 清空当帧样本 ----
auto test_frame_protocol() -> void {
    Profiler &p = fresh();

    AURORA_TEST_CHECK_MSG(p.frame_index() == 0, "Test1: frame_index == 0 after reset");

    p.begin_frame();
    p.begin_zone("a");
    p.end_zone();
    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 1, "Test1: frame sample count == 1 after closing one zone");
    p.end_frame();

    AURORA_TEST_CHECK_MSG(p.frame_index() == 1, "Test1: frame_index == 1 after end_frame");
    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 1,
                          "Test1: end_frame does not clear samples (for post-frame snapshot)");

    p.begin_frame();
    AURORA_TEST_CHECK_MSG(p.frame_zones().empty(), "Test1: begin_frame clears previous frame samples");
    p.end_frame();
    AURORA_TEST_CHECK_MSG(p.frame_index() == 2, "Test1: frame_index == 2 after second frame");
}

// ---- Test 2: 嵌套深度按闭合顺序记录（内层先闭合，depth 更大）----
auto test_nesting_depth() -> void {
    Profiler &p = fresh();
    p.begin_frame();

    p.begin_zone("outer");
    p.begin_zone("inner");
    p.end_zone(); // inner 先闭合
    p.end_zone(); // outer 后闭合

    p.end_frame();

    const auto &zones = p.frame_zones();
    AURORA_TEST_CHECK_MSG(zones.size() == 2, "Test2: recorded 2 zones");
    if (zones.size() == 2) {
        AURORA_TEST_CHECK_MSG(std::string(zones[0].name) == "inner", "Test2: first entry is the first-closed inner");
        AURORA_TEST_CHECK_MSG(zones[0].depth == 1, "Test2: inner depth == 1");
        AURORA_TEST_CHECK_MSG(std::string(zones[1].name) == "outer", "Test2: second entry is the last-closed outer");
        AURORA_TEST_CHECK_MSG(zones[1].depth == 0, "Test2: outer depth == 0");
        AURORA_TEST_CHECK_MSG(zones[1].duration_ms >= zones[0].duration_ms,
                              "Test2: outer duration >= inner (containment)");
        AURORA_TEST_CHECK_MSG(zones[0].start_ms >= 0.0, "Test2: start_ms non-negative relative to frame start");
    }
}

// ---- Test 3: aggregate / aggregates 聚合 ----
auto test_aggregate() -> void {
    Profiler &p = fresh();
    p.begin_frame();

    for (int i = 0; i < 3; ++i) {
        p.begin_zone("paint");
        p.end_zone();
    }
    p.begin_zone("layout");
    busy_wait_ms(2.0); // 让 layout 明显比 paint 慢，验证聚合列表按总耗时降序
    p.end_zone();

    p.end_frame();

    const ZoneAggregate paint = p.aggregate("paint");
    AURORA_TEST_CHECK_MSG(paint.call_count == 3, "Test3: paint aggregate call_count == 3");
    AURORA_TEST_CHECK_MSG(paint.total_ms >= paint.max_ms, "Test3: total_ms >= max_ms");

    const ZoneAggregate missing = p.aggregate("nonexistent");
    AURORA_TEST_CHECK_MSG(missing.call_count == 0, "Test3: aggregate call_count == 0 for absent zone");

    const auto all = p.aggregates();
    AURORA_TEST_CHECK_MSG(all.size() == 2, "Test3: aggregates() returns 2 distinct names");
    if (!all.empty()) {
        AURORA_TEST_CHECK_MSG(std::string(all[0].name) == "layout",
                              "Test3: aggregates() sorted by total time desc (layout first)");
    }
}

// ---- Test 4: 长任务判定与跨帧累计 ----
auto test_long_tasks() -> void {
    Profiler &p = fresh();
    p.set_long_task_threshold_ms(3.0);
    AURORA_TEST_CHECK_MSG(near_d(p.long_task_threshold_ms(), 3.0, 1e-9), "Test4: threshold set takes effect");

    p.begin_frame();
    p.begin_zone("slow");
    busy_wait_ms(5.0);
    p.end_zone();
    p.begin_zone("fast");
    p.end_zone();
    p.end_frame();

    AURORA_TEST_CHECK_MSG(p.long_tasks().size() == 1,
                          "Test4: current-frame long-task count == 1 (only slow exceeds threshold)");
    if (p.long_tasks().size() == 1) {
        AURORA_TEST_CHECK_MSG(std::string(p.long_tasks()[0].name) == "slow", "Test4: long task attributed to slow");
    }
    AURORA_TEST_CHECK_MSG(p.total_long_task_count() == 1, "Test4: cross-frame accumulation == 1");

    p.begin_frame();
    AURORA_TEST_CHECK_MSG(p.long_tasks().empty(), "Test4: begin_frame clears current-frame long-task list");
    p.begin_zone("slow2");
    busy_wait_ms(5.0);
    p.end_zone();
    p.end_frame();

    AURORA_TEST_CHECK_MSG(p.total_long_task_count() == 2,
                          "Test4: cross-frame accumulation not cleared by begin_frame, == 2");

    p.reset();
    AURORA_TEST_CHECK_MSG(p.total_long_task_count() == 0, "Test4: reset clears cross-frame accumulation");
}

// ---- Test 5: 容量上限——超出即丢弃并计数，不扩容 ----
auto test_capacity_drop() -> void {
    Profiler &p = fresh();
    p.set_zone_capacity(4);
    AURORA_TEST_CHECK_MSG(p.zone_capacity() == 4, "Test5: zone_capacity() == 4");

    p.begin_frame();
    for (int i = 0; i < 10; ++i) {
        p.begin_zone("z");
        p.end_zone();
    }
    p.end_frame();

    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 4, "Test5: sample count truncated to 4 by capacity");
    AURORA_TEST_CHECK_MSG(p.dropped_zones() == 6, "Test5: dropped_zones() == 6");

    p.set_zone_capacity(Profiler::AURORA_DEFAULT_ZONE_CAPACITY); // 复位，避免污染后续用例
}

// ---- Test 6: 配对错误检测 ----
auto test_unbalanced() -> void {
    Profiler &p = fresh();

    // (a) end 多于 begin
    p.begin_frame();
    p.end_zone();
    p.end_frame();
    AURORA_TEST_CHECK_MSG(p.unbalanced_zones() >= 1, "Test6a: end outnumbering begin counted as unbalanced");

    // (b) 帧末仍有未闭合 zone
    p.reset();
    p.begin_frame();
    p.begin_zone("leaked");
    p.end_frame(); // 未 end_zone
    AURORA_TEST_CHECK_MSG(p.unbalanced_zones() == 1, "Test6b: unclosed zone at frame end counted as unbalanced");

    // (c) 强制复位后不跨帧传播
    p.begin_frame();
    p.begin_zone("ok");
    p.end_zone();
    p.end_frame();
    AURORA_TEST_CHECK_MSG(p.unbalanced_zones() == 1,
                          "Test6c: errors do not accumulate across frames (normal pairing next frame adds none)");
    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 1, "Test6c: next frame records samples normally");
}

// ---- Test 7: 运行时开关 ----
auto test_runtime_switch() -> void {
    Profiler &p = fresh();

    p.set_enabled(false);
    AURORA_TEST_CHECK_MSG(!p.is_enabled(), "Test7: set_enabled(false) takes effect");

    p.begin_frame();
    p.begin_zone("ignored");
    p.end_zone();
    p.end_frame();
    AURORA_TEST_CHECK_MSG(p.frame_zones().empty(), "Test7: no samples produced when disabled");
    AURORA_TEST_CHECK_MSG(p.unbalanced_zones() == 0,
                          "Test7: begin/end ignored in pairs when disabled (no false pairing error)");

    p.set_enabled(true);
    p.begin_frame();
    p.begin_zone("counted");
    p.end_zone();
    p.end_frame();
    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 1, "Test7: sampling resumes after re-enabling");
}

// ---- Test 8: ScopedTimer RAII ----
auto test_scoped_timer() -> void {
    Profiler &p = fresh();
    p.begin_frame();
    {
        const ScopedTimer t{ "scoped" };
        busy_wait_ms(1.0);
    }
    p.end_frame();

    const ZoneAggregate agg = p.aggregate("scoped");
    AURORA_TEST_CHECK_MSG(agg.call_count == 1, "Test8: ScopedTimer produces 1 sample after destruction");
    AURORA_TEST_CHECK_MSG(agg.total_ms >= 1.0, "Test8: sample duration covers busy-wait within scope");
}

// ---- Test 9: FrameScope 同时管理帧协议与计数器归零 ----
auto test_frame_scope() -> void {
    Profiler &p = fresh();
    RenderCounters &c = RenderCounters::current();
    c.draw_calls = 12345; // 故意留下上一帧的脏值

    const std::uint64_t before = p.frame_index();
    {
        const FrameScope fs;
        AURORA_TEST_CHECK_MSG(c.draw_calls == 0, "Test9: FrameScope zeroes RenderCounters on construction");
        c.draw_calls = 7;
        p.begin_zone("in-frame");
        p.end_zone();
    }
    AURORA_TEST_CHECK_MSG(p.frame_index() == before + 1, "Test9: FrameScope advances frame index on destruction");
    AURORA_TEST_CHECK_MSG(p.frame_zones().size() == 1,
                          "Test9: current-frame samples still readable after destruction (for post-frame snapshot)");
    AURORA_TEST_CHECK_MSG(c.draw_calls == 7, "Test9: destruction does not zero counters (cleared on next begin_frame)");

    c.reset();
}

// ---- Test 10: reset 语义（清状态、保配置）----
auto test_reset_semantics() -> void {
    Profiler &p = fresh();
    p.set_zone_capacity(64);
    p.set_long_task_threshold_ms(1.0);

    p.begin_frame();
    p.begin_zone("x");
    busy_wait_ms(2.0);
    p.end_zone();
    p.end_frame();

    p.reset();

    AURORA_TEST_CHECK_MSG(p.frame_index() == 0, "Test10: reset clears frame_index");
    AURORA_TEST_CHECK_MSG(p.frame_zones().empty(), "Test10: reset clears current-frame samples");
    AURORA_TEST_CHECK_MSG(p.long_tasks().empty(), "Test10: reset clears current-frame long tasks");
    AURORA_TEST_CHECK_MSG(p.dropped_zones() == 0, "Test10: reset clears dropped");
    AURORA_TEST_CHECK_MSG(p.unbalanced_zones() == 0, "Test10: reset clears unbalanced");
    AURORA_TEST_CHECK_MSG(p.zone_capacity() == 64, "Test10: reset preserves capacity config");
    AURORA_TEST_CHECK_MSG(near_d(p.long_task_threshold_ms(), 1.0, 1e-9), "Test10: reset preserves threshold config");

    p.set_zone_capacity(Profiler::AURORA_DEFAULT_ZONE_CAPACITY);
    p.set_long_task_threshold_ms(Profiler::AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS);
}

// ---- Test 11: report_text 结构 ----
auto test_report_text() -> void {
    Profiler &p = fresh();
    p.begin_frame();
    p.begin_zone("alpha");
    p.end_zone();
    p.end_frame();

    const std::string txt = p.report_text();
    AURORA_TEST_CHECK_MSG(txt.find("zone") != std::string::npos, "Test11: report_text contains header");
    AURORA_TEST_CHECK_MSG(txt.find("alpha") != std::string::npos, "Test11: report_text contains zone name");
    AURORA_TEST_CHECK_MSG(txt.back() != '\n', "Test11: report_text has no trailing newline (caller appends)");
}

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_perf_profiler ===\n");

    test_frame_protocol();
    test_nesting_depth();
    test_aggregate();
    test_long_tasks();
    test_capacity_drop();
    test_unbalanced();
    test_runtime_switch();
    test_scoped_timer();
    test_frame_scope();
    test_reset_semantics();
    test_report_text();

    // 归还单例到默认状态：同一进程内本文件是唯一使用者，但保持卫生。
    fresh();
}
