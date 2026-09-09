/// 测试类型: unit
/// 目标单元: include/aurora/perf/profiler.h
/// 测试说明: 覆盖 Profiler 单例——帧协议（begin/end_frame 与帧序号推进）、作用域配对与嵌套深度、
/// 同名 zone 聚合（按内容比较）、report_text、长任务阈值判定、运行时开关、容量溢出丢弃、
/// 配对错误计数（unbalanced）、ScopedTimer/FrameScope RAII 语义。
/// 类方法不受 AURORA_ENABLE_PROFILING 宏裁切（仅埋点宏被裁切），故全部用例可直接驱动；
/// 依赖插桩分支的语义（如 PerfSession 的 zone 合并）由各用例运行时探测 profiling_enabled()。

#include <cstdint>
#include <vector>

#include "aurora/perf/profiler.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_profiler {

namespace {

/// @brief 把单例恢复到默认配置并清空状态：用例间互不污染（容量与阈值保留语义被显式复位）。
auto reset_profiler_to_defaults() -> void {
    Profiler& prof = Profiler::instance();
    prof.reset();
    prof.set_enabled(true);
    prof.set_long_task_threshold_ms(Profiler::AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS);
    prof.set_zone_capacity(Profiler::AURORA_DEFAULT_ZONE_CAPACITY);
    RenderCounters::current().reset();
}

}  // namespace

AURORA_TEST_CASE(zone_sample_is_recorded_with_defaults) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    AURORA_TEST_CHECK_EQ(prof.frame_index(), std::uint64_t{0});
    prof.begin_frame();
    prof.begin_zone("zone_a");
    prof.end_zone();
    prof.end_frame();

    // 帧序号在 end_frame 推进；样本按闭合顺序入列，时序字段非负。
    AURORA_TEST_CHECK_EQ(prof.frame_index(), std::uint64_t{1});
    const std::vector<ZoneSample>& zones = prof.frame_zones();
    AURORA_TEST_CHECK_EQ(zones.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(zones[0].name, "zone_a");
    AURORA_TEST_CHECK_EQ(zones[0].depth, std::uint16_t{0});
    AURORA_TEST_CHECK_GE(zones[0].duration_ms, 0.0);
    AURORA_TEST_CHECK_GE(zones[0].start_ms, 0.0);
}

AURORA_TEST_CASE(nested_zones_have_increasing_depth) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    prof.begin_frame();
    prof.begin_zone("outer");
    prof.begin_zone("inner");
    prof.end_zone();
    prof.end_zone();
    prof.end_frame();

    // 按闭合顺序：内层先出栈（depth 1），外层后出栈（depth 0）。
    const std::vector<ZoneSample>& zones = prof.frame_zones();
    AURORA_TEST_CHECK_EQ(zones.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(zones[0].name, "inner");
    AURORA_TEST_CHECK_EQ(zones[0].depth, std::uint16_t{1});
    AURORA_TEST_CHECK_STREQ(zones[1].name, "outer");
    AURORA_TEST_CHECK_EQ(zones[1].depth, std::uint16_t{0});
}

AURORA_TEST_CASE(aggregate_merges_same_name_within_frame) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    prof.begin_frame();
    prof.begin_zone("dup");
    prof.end_zone();
    prof.begin_zone("dup");
    prof.end_zone();
    prof.begin_zone("other");
    prof.end_zone();
    prof.end_frame();

    const ZoneAggregate dup = prof.aggregate("dup");
    AURORA_TEST_CHECK_EQ(dup.call_count, std::uint32_t{2});
    AURORA_TEST_CHECK_GE(dup.total_ms, 0.0);
    AURORA_TEST_CHECK_GE(dup.max_ms, 0.0);
    AURORA_TEST_CHECK_LE(dup.max_ms, dup.total_ms + 1e-9);

    // 无匹配名字：返回 call_count == 0 的空结果。
    AURORA_TEST_CHECK_EQ(prof.aggregate("missing").call_count, std::uint32_t{0});

    // 全量聚合按名字分桶（2 个桶），按总耗时降序排列。
    const std::vector<ZoneAggregate> aggs = prof.aggregates();
    AURORA_TEST_CHECK_EQ(aggs.size(), std::size_t{2});
    for (std::size_t i = 1; i < aggs.size(); ++i) {
        AURORA_TEST_CHECK_GE(aggs[i - 1].total_ms, aggs[i].total_ms);
    }
}

AURORA_TEST_CASE(aggregate_name_compared_by_content) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // 同名 zone 可能来自不同 TU 的不同地址：聚合按内容而非指针比较。
    // 用栈上数组构造一个与字面量地址不同的同内容名字。
    char stack_dup[4] = {'d', 'u', 'p', '\0'};
    prof.begin_frame();
    prof.begin_zone("dup");
    prof.end_zone();
    prof.begin_zone(stack_dup);
    prof.end_zone();
    prof.end_frame();

    AURORA_TEST_CHECK_EQ(prof.aggregate("dup").call_count, std::uint32_t{2});
}

AURORA_TEST_CASE(report_text_lists_zone_names) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    prof.begin_frame();
    prof.begin_zone("layout_pass");
    prof.end_zone();
    prof.end_frame();

    const std::string text = prof.report_text();
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr("layout_pass"));
    AURORA_TEST_CHECK_FALSE(text.empty());
}

AURORA_TEST_CASE(end_zone_without_begin_is_unbalanced) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // end 多于 begin：计入配对错误。
    prof.end_zone();
    AURORA_TEST_CHECK_EQ(prof.unbalanced_zones(), std::uint64_t{1});
}

AURORA_TEST_CASE(unclosed_zone_at_frame_end_is_unbalanced) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // 帧边界残留未闭合 zone：计入配对错误并强制复位（错误不跨帧传播）。
    prof.begin_frame();
    prof.begin_zone("leak");
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.unbalanced_zones(), std::uint64_t{1});

    // 下一帧从干净状态开始：正常采一个 zone 只产生一个样本。
    prof.begin_frame();
    prof.begin_zone("fresh");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.frame_zones().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(prof.frame_zones()[0].name, "fresh");
}

AURORA_TEST_CASE(long_task_detection_respects_threshold) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // 阈值 0：任何 zone（耗时 >= 0）都判为长任务。
    prof.set_long_task_threshold_ms(0.0);
    AURORA_TEST_CHECK_NEAR(prof.long_task_threshold_ms(), 0.0, 1e-9);
    prof.begin_frame();
    prof.begin_zone("slow");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.long_tasks().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(prof.long_tasks()[0].name, "slow");
    AURORA_TEST_CHECK_EQ(prof.total_long_task_count(), std::uint64_t{1});

    // 阈值天文数字：不再产生长任务（当帧列表在 begin_frame 已清空）。
    prof.begin_frame();
    prof.set_long_task_threshold_ms(1.0e12);
    prof.begin_zone("fast");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_TRUE(prof.long_tasks().empty());
    AURORA_TEST_CHECK_EQ(prof.total_long_task_count(), std::uint64_t{1});  // 跨帧累计只增
}

AURORA_TEST_CASE(runtime_disable_suppresses_zones) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // 运行时二级开关关闭：begin/end 立即返回，不产生样本也不误计配对错误。
    prof.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(prof.is_enabled());
    prof.begin_frame();
    prof.begin_zone("suppressed");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_TRUE(prof.frame_zones().empty());
    AURORA_TEST_CHECK_EQ(prof.unbalanced_zones(), std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(prof.dropped_zones(), std::uint64_t{0});

    // 重新开启后恢复采集。
    prof.set_enabled(true);
    prof.begin_frame();
    prof.begin_zone("visible");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.frame_zones().size(), std::size_t{1});
}

AURORA_TEST_CASE(zone_capacity_overflow_drops_samples) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // 容量 1：第二个样本被丢弃并计数，已采样本保留。
    prof.set_zone_capacity(1);
    AURORA_TEST_CHECK_EQ(prof.zone_capacity(), std::size_t{1});
    prof.begin_frame();
    prof.begin_zone("first");
    prof.end_zone();
    prof.begin_zone("second");
    prof.end_zone();
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.frame_zones().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(prof.frame_zones()[0].name, "first");
    AURORA_TEST_CHECK_GE(prof.dropped_zones(), std::uint64_t{1});
}

AURORA_TEST_CASE(frame_scope_manages_frame_protocol_and_counters) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // FrameScope 构造：开帧并清零当帧渲染计数器；析构：闭帧（帧序号推进）。
    RenderCounters::current().draw_calls = 9;
    {
        aurora::FrameScope frame;
        AURORA_TEST_CHECK_EQ(RenderCounters::current().draw_calls, std::uint32_t{0});
        prof.begin_zone("in_frame");
        prof.end_zone();
    }
    AURORA_TEST_CHECK_EQ(prof.frame_index(), std::uint64_t{1});
    // 析构后数据仍可读（begin_frame 才清空当帧样本）。
    AURORA_TEST_CHECK_EQ(prof.frame_zones().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(prof.frame_zones()[0].name, "in_frame");
}

AURORA_TEST_CASE(scoped_timer_is_raii_zone) {
    reset_profiler_to_defaults();
    Profiler& prof = Profiler::instance();

    // ScopedTimer：构造进入 zone、析构离开 zone，与手动 begin/end 等价。
    prof.begin_frame();
    {
        aurora::ScopedTimer timer{"raii_zone"};
        AURORA_TEST_CHECK_TRUE(prof.frame_zones().empty());  // 未闭合前不产生样本
    }
    prof.end_frame();
    AURORA_TEST_CHECK_EQ(prof.frame_zones().size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(prof.frame_zones()[0].name, "raii_zone");
}

}  // namespace aurora::test_cases::utest_profiler
