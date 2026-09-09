/// 测试类型: unit
/// 目标单元: include/aurora/debug/debug_runtime.h
/// 测试说明: 覆盖运行时信息导出门面五项能力——widget_tree（委托 Inspector::tree_json_full 的
/// 等价性与结构）、perf_snapshot / frame_phase_timeline（关键键与 limit 语义）、diagnostics、
/// why_trace 快照形状，以及关闭态（未开 AURORA_ENABLE_DEBUG）统一返回
/// {"available":false,"reason":...} 的 disabled 语义。测试 TU 不写宏门控：
/// 以 feature_flags().debug 运行时探测，探测失败即 SKIP/分支。

#include <string>

#include "aurora/debug/debug_runtime.h"
#include "aurora/debug/feature_flags.h"  // 运行时探测 AURORA_ENABLE_DEBUG 的归一化镜像
#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_debug_runtime {

using aurora::debug::diagnostics;
using aurora::debug::feature_flags;
using aurora::debug::frame_phase_timeline;
using aurora::debug::perf_snapshot;
using aurora::debug::why_trace;
using aurora::debug::widget_tree;

/// @brief 构造确定性测试树：Column 根 + 单个 Text 子节点（无需布局即可序列化）。
[[nodiscard]] auto make_tree() -> Node {
    auto col = std::make_shared<Column>();
    col->add(Node{std::make_shared<Text>("hi")});
    return Node{col};
}

/// @brief 运行时探测 AURORA_ENABLE_DEBUG 是否生效（feature_flags 为始终可用的编译期快照）。
[[nodiscard]] auto probe_debug_enabled() -> bool { return feature_flags().debug; }

AURORA_TEST_CASE(facade_functions_return_unavailable_when_debug_off) {
    if (probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 已启用：关闭态 disabled 语义不适用");
    }
    // Release 契约：五项能力统一返回 {"available":false,"reason":...}，零调试代码可观测。
    const Json tree = widget_tree(make_tree());
    AURORA_TEST_CHECK_EQ(tree["available"], false);
    AURORA_TEST_CHECK_TRUE(tree.contains("reason"));
    const Json snapshots[4] = {perf_snapshot(), frame_phase_timeline(), why_trace(), diagnostics()};
    for (const Json& j : snapshots) {
        AURORA_TEST_CHECK_EQ(j["available"], false);
        AURORA_TEST_CHECK_TRUE(j.contains("reason"));
    }
}

AURORA_TEST_CASE(widget_tree_delegates_to_inspector_full_json) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：widget_tree 按宏裁切返回 unavailable");
    }
    // 门面收编原则：widget_tree 是 Inspector::tree_json_full 的薄封装，输出必须等价。
    Node root = make_tree();
    const Json via_facade = widget_tree(root);
    const Json via_inspector = Inspector::tree_json_full(root);
    AURORA_TEST_CHECK_EQ(via_facade.dump(), via_inspector.dump());
}

AURORA_TEST_CASE(widget_tree_json_structure) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：widget_tree 按宏裁切返回 unavailable");
    }
    const Json j = widget_tree(make_tree());
    AURORA_TEST_CHECK_EQ(j["type"], "Column");
    AURORA_TEST_CHECK_TRUE(j["props"].is_object());
    AURORA_TEST_CHECK_EQ(j["children"].size(), 1U);
    AURORA_TEST_CHECK_EQ(j["children"][0]["type"], "Text");
    AURORA_TEST_CHECK_EQ(j["children"][0]["props"]["content"], "hi");
}

AURORA_TEST_CASE(perf_snapshot_exposes_frame_stats_keys) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：perf_snapshot 按宏裁切返回 unavailable");
    }
    const Json j = perf_snapshot();
    // 关键读数键齐全（聚合 FrameStats + PerfLog::snapshot_json）。
    AURORA_TEST_CHECK_TRUE(j.contains("fps"));
    AURORA_TEST_CHECK_TRUE(j.contains("avg_frame_ms"));
    AURORA_TEST_CHECK_TRUE(j.contains("worst_frame_ms"));
    AURORA_TEST_CHECK_TRUE(j.contains("total_frames"));
    AURORA_TEST_CHECK_TRUE(j.contains("frame_budget_ms"));
    AURORA_TEST_CHECK_TRUE(j.contains("perf_log"));
    AURORA_TEST_CHECK_TRUE(j["fps"].is_number());
    AURORA_TEST_CHECK_TRUE(j["total_frames"].is_number());
}

AURORA_TEST_CASE(frame_phase_timeline_respects_limit) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：timeline 按宏裁切返回 unavailable");
    }
    const Json j = frame_phase_timeline(4);
    AURORA_TEST_CHECK_TRUE(j.contains("avg_layout_ms"));
    AURORA_TEST_CHECK_TRUE(j.contains("avg_paint_ms"));
    AURORA_TEST_CHECK_TRUE(j.contains("avg_present_ms"));
    // recent_frame_ms 为最近帧时间窗口（本用例未跑帧，空数组也须 ≤ limit）。
    AURORA_TEST_CHECK_TRUE(j["recent_frame_ms"].is_array());
    AURORA_TEST_CHECK_LE(j["recent_frame_ms"].size(), 4U);
    // ASCII flamegraph 为非空字符串。
    AURORA_TEST_CHECK_TRUE(j["flamegraph"].is_string());
    AURORA_TEST_CHECK_GT(j["flamegraph"].get<std::string>().size(), 0U);
}

AURORA_TEST_CASE(diagnostics_snapshot_shape) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：diagnostics 按宏裁切返回 unavailable");
    }
    const Json j = diagnostics();
    AURORA_TEST_CHECK_TRUE(j["count"].is_number());
    AURORA_TEST_CHECK_TRUE(j["diagnostics"].is_array());
    AURORA_TEST_CHECK_EQ(j["diagnostics"].size(), j["count"]);
}

AURORA_TEST_CASE(why_trace_snapshot_shape) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：why_trace 按宏裁切返回 unavailable");
    }
    const Json j = why_trace();
    AURORA_TEST_CHECK_TRUE(j.contains("count"));
    AURORA_TEST_CHECK_TRUE(j.contains("total_recorded"));
    AURORA_TEST_CHECK_TRUE(j["entries"].is_array());
}

AURORA_TEST_CASE(facade_functions_never_throw_with_live_tree) {
    // 双构建通用：开启态传真实树全链路安全；关闭态走 disabled 早退路径同样安全。
    Node root = make_tree();
    AURORA_TEST_CHECK_NO_THROW((void)widget_tree(root));
    AURORA_TEST_CHECK_NO_THROW((void)perf_snapshot());
    AURORA_TEST_CHECK_NO_THROW((void)frame_phase_timeline(8));
    AURORA_TEST_CHECK_NO_THROW((void)why_trace(8));
    AURORA_TEST_CHECK_NO_THROW((void)diagnostics());
}

}  // namespace aurora::test_cases::utest_debug_runtime
