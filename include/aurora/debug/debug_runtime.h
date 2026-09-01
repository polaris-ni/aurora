#pragma once

// 运行时信息导出门面（specification/06-app-platform.md §11.2）。
//
// 五项能力：widget_tree / perf_snapshot / frame_phase_timeline / why_trace / diagnostics。
// 设计要点（与 debug_backend.h / debug_paint.h 一致的双门控）：
// - 本头**始终声明** `aurora::debug` 函数，使消费端调用可编译、ODR 安全。
// - 函数**体**按 `AURORA_ENABLE_DEBUG` 裁切：Release（未开开关）下返回
//   `{"available":false, "reason": ...}`（why_trace 等同），零调试代码被编译进 Release。
// - 收编原则：本文件是门面，不搬迁任何生产子系统引擎（Inspector / perf / Diagnostics 留原地），
//   仅做薄封装 / 聚合。唯一真正进入 `aurora::debug` 的是 `record_dirty` 的采集入口
//  （由 engine 热路径在 DEBUG 下调用），以及 why_trace 的只读缓冲。

#include "aurora/widget/props_io.h"

namespace aurora {

class Node; // 前向声明（widget_tree 入参按引用使用）

namespace debug {

/// @brief Widget 树完整 JSON 快照（type + props + children）。
///        薄封装 `Inspector::tree_json_full`（引擎留 `inspector/`，不搬迁）。
/// @note Release（未开 DEBUG）返回 `{"available":false, "reason": ...}`。
[[nodiscard]] auto widget_tree(const Node &root) -> Json;

/// @brief 性能快照 JSON：聚合 `FrameStats` 读数 + `PerfLog::snapshot_json()`。
/// @note Release 返回 `{"available":false, ...}`。
[[nodiscard]] auto perf_snapshot() -> Json;

/// @brief 帧相位时间线 JSON：layout / paint / present 各相位平均耗时、帧时间统计与 ASCII
///        flamegraph（复用 `FrameStats` 已维护的相位环形缓冲，零新埋点）。
/// @param limit 帧时间窗口最多取最近多少帧（毫秒）进 `recent_frame_ms`。
/// @note Release 返回 `{"available":false, ...}`。
[[nodiscard]] auto frame_phase_timeline(std::size_t limit = 64) -> Json;

/// @brief why-relayout / why-repaint 追踪 JSON：最近 `limit` 次 `mark_needs_layout` /
///        `mark_needs_paint` 触发记录（kind / type / frame / propagated），区分根因与父链传播。
/// @note Release 返回 `{"available":false, ...}`（热路径未记录）。
[[nodiscard]] auto why_trace(std::size_t limit = 64) -> Json;

/// @brief 运行时诊断只读快照 JSON：薄封装 `Diagnostics::get_last_diagnostics()`。
/// @note Release 返回 `{"available":false, ...}`。
[[nodiscard]] auto diagnostics() -> Json;

} // namespace debug
} // namespace aurora
