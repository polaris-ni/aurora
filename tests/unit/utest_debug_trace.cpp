/// 测试类型: unit
/// 目标单元: include/aurora/debug/debug_trace.h
/// 测试说明: 覆盖 why_trace 热路径埋点——DirtyKind 枚举、record_dirty 采集 + why_trace
/// 查询往返（count/total_recorded 增量、entries 最新在前、kind/type/frame/propagated 字段）、
/// limit 截断语义、关闭态（未开 AURORA_ENABLE_DEBUG）unavailable JSON 与 no-op 安全性。
/// 测试 TU 不写宏门控：以 feature_flags().debug 运行时探测，探测失败即 SKIP/分支。

#include <cstdint>
#include <string>
#include <type_traits>

#include "aurora/debug/debug_runtime.h"  // why_trace 查询门面（与 record_dirty 同域）
#include "aurora/debug/debug_trace.h"
#include "aurora/debug/feature_flags.h"  // 运行时探测 AURORA_ENABLE_DEBUG 的归一化镜像
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_debug_trace {

using aurora::debug::feature_flags;
using aurora::debug::why_trace;

/// @brief 运行时探测 AURORA_ENABLE_DEBUG 是否生效（feature_flags 为始终可用的编译期快照）。
[[nodiscard]] auto probe_debug_enabled() -> bool { return feature_flags().debug; }

AURORA_TEST_CASE(dirty_kind_enumerates_layout_and_paint) {
    // 契约：脏标记恰有两种——重排 / 重绘（underlying 为 uint8_t）。
    static_assert(std::is_enum_v<aurora::debug::DirtyKind>, "DirtyKind 必须是 enum class");
    const auto layout = static_cast<std::uint8_t>(aurora::debug::DirtyKind::Layout);
    const auto paint = static_cast<std::uint8_t>(aurora::debug::DirtyKind::Paint);
    AURORA_TEST_CHECK_NE(layout, paint);
}

AURORA_TEST_CASE(why_trace_reports_unavailable_when_debug_off) {
    if (probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 已启用：关闭态 unavailable 语义不适用");
    }
    const Json j = why_trace();
    AURORA_TEST_CHECK_EQ(j["available"], false);
    AURORA_TEST_CHECK_TRUE(j.contains("reason"));
    // 关闭态热路径未记录：查询仍安全返回结构化 JSON（始终声明、ODR 安全契约）。
    AURORA_TEST_CHECK_TRUE(j.is_object());
}

AURORA_TEST_CASE(record_dirty_never_throws_in_any_build) {
    // 注入点语义：头文件始终声明，任何构建下调用皆安全（关闭态为 no-op）。
    AURORA_TEST_CHECK_NO_THROW(aurora::debug::detail::record_dirty(
        aurora::debug::DirtyKind::Layout, "Text", 1, false));
}

AURORA_TEST_CASE(record_and_query_roundtrip) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：采集缓冲按宏裁切，无记录可查");
    }
    const Json before = why_trace();
    const auto base_count = before["count"].get<std::size_t>();
    const auto base_total = before["total_recorded"].get<std::uint64_t>();

    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Layout, "Text", 101, false);
    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Paint, "Button", 102, true);
    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Layout, "Column", 103, false);

    const Json after = why_trace();
    AURORA_TEST_CHECK_EQ(after["count"], base_count + 3);
    AURORA_TEST_CHECK_EQ(after["total_recorded"], base_total + 3);

    // entries 最新在前：最后记录的 Column 排首位；字段逐项核对。
    const Json &entries = after["entries"];
    AURORA_TEST_REQUIRE_EQ(entries.size(), 3U);
    AURORA_TEST_CHECK_EQ(entries[0]["kind"], "layout");
    AURORA_TEST_CHECK_EQ(entries[0]["type"], "Column");
    AURORA_TEST_CHECK_EQ(entries[0]["frame"], 103);
    AURORA_TEST_CHECK_EQ(entries[0]["propagated"], false);
    AURORA_TEST_CHECK_EQ(entries[1]["kind"], "paint");
    AURORA_TEST_CHECK_EQ(entries[1]["type"], "Button");
    AURORA_TEST_CHECK_EQ(entries[1]["frame"], 102);
    AURORA_TEST_CHECK_EQ(entries[1]["propagated"], true);
    AURORA_TEST_CHECK_EQ(entries[2]["kind"], "layout");
    AURORA_TEST_CHECK_EQ(entries[2]["type"], "Text");
    AURORA_TEST_CHECK_EQ(entries[2]["frame"], 101);
    AURORA_TEST_CHECK_EQ(entries[2]["propagated"], false);
}

AURORA_TEST_CASE(why_trace_limit_keeps_newest_first) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：limit 截断语义仅开启态可观测");
    }
    const Json before = why_trace();
    const auto base_count = before["count"].get<std::size_t>();

    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Layout, "A", 201, false);
    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Paint, "B", 202, false);
    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Layout, "C", 203, false);

    // limit=2：只取最近 2 条，仍最新在前。
    const Json limited = why_trace(2);
    AURORA_TEST_CHECK_EQ(limited["count"], base_count + 3);  // count 不受 limit 影响
    const Json &entries = limited["entries"];
    AURORA_TEST_REQUIRE_EQ(entries.size(), 2U);
    AURORA_TEST_CHECK_EQ(entries[0]["type"], "C");
    AURORA_TEST_CHECK_EQ(entries[0]["frame"], 203);
    AURORA_TEST_CHECK_EQ(entries[1]["type"], "B");
    AURORA_TEST_CHECK_EQ(entries[1]["frame"], 202);

    // limit=0：entries 为空，缓冲本身不受影响。
    const Json zero = why_trace(0);
    AURORA_TEST_CHECK_EQ(zero["entries"].size(), 0U);
    AURORA_TEST_CHECK_EQ(zero["count"], base_count + 3);
}

AURORA_TEST_CASE(total_recorded_is_monotonic) {
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：累计计数按宏裁切");
    }
    const auto base_total = why_trace()["total_recorded"].get<std::uint64_t>();
    aurora::debug::detail::record_dirty(aurora::debug::DirtyKind::Paint, "Text", 301, true);
    const auto after_total = why_trace()["total_recorded"].get<std::uint64_t>();
    AURORA_TEST_CHECK_GE(after_total, base_total + 1);
}

}  // namespace aurora::test_cases::utest_debug_trace
