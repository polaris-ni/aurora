#pragma once

// why_trace 热路径埋点声明（specification/06-app-platform.md §11.2 / why_trace）。
//
// 设计要点：
// - 极轻量：仅前向声明枚举与记录函数，**不 include `widget.h`**，避免与 `Widget` 形成
//   include 循环（`widget.h` 的 `mark_needs_layout` / `mark_needs_paint` 内联函数需要调用它）。
// - 记录函数体按 `AURORA_ENABLE_DEBUG` 在 `debug_runtime.cpp` 裁切；Release 下 `widget.h` 的
//   调用点被宏包裹不编译，故此声明在 Release 仅被前向引用、无定义需求（不存在 ODR-use）。

#include <cstdint>

namespace aurora::debug {

/// @brief 脏标记种类：重排 / 重绘。
enum class DirtyKind : std::uint8_t { Layout, Paint };

namespace detail {

/// @brief 在 `mark_needs_layout` / `mark_needs_paint` 热路径（仅 `AURORA_ENABLE_DEBUG` 下）记录一次触发。
/// @param kind        Layout / Paint。
/// @param type_name   触发控件的 `type_name()`。
/// @param frame       当前调试帧（`aurora::debug::current_debug_frame()`）。
/// @param propagated  是否由父链传播（true=引擎自动沿父链冒泡，false=业务/状态直接触发的根因）。
/// @note Release 构建下本函数无调用方（`widget.h` 调用点被 `#ifdef` 包裹），不会产生未定义引用。
auto record_dirty(DirtyKind kind, const char *type_name, std::uint64_t frame, bool propagated) -> void;

} // namespace detail
} // namespace aurora::debug
