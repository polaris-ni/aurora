#pragma once

#include <cstdint>

#include "aurora/core/types.h"

namespace aurora {

/**
 * @brief 对齐方式（九宫格）：控制子项在可用空间内的定位。
 *
 * 单独成文件以避免 `modifier.h` ↔ `widget/stack.h` 的循环包含
 * （`modifier` 需 `Alignment` 实现 `align()`，而 `stack.h` 也产出 `Stack`）。
 *
 * @note Thread: thread-safe (pure enum)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
enum class Alignment : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

/// @brief 返回子项相对容器的对齐原点（左上角），使子项按 `a` 定位。
inline auto align_origin(Alignment a, Size child_size, Size container_size) -> Point {
    const float cx = (container_size.width - child_size.width) * 0.5f;
    const float cy = (container_size.height - child_size.height) * 0.5f;
    switch (a) {
    case Alignment::TopLeft: return Point{ .x = 0.0f, .y = 0.0f };
    case Alignment::TopCenter: return Point{ .x = cx, .y = 0.0f };
    case Alignment::TopRight: return Point{ .x = container_size.width - child_size.width, .y = 0.0f };
    case Alignment::CenterLeft: return Point{ .x = 0.0f, .y = cy };
    case Alignment::Center: return Point{ .x = cx, .y = cy };
    case Alignment::CenterRight: return Point{ .x = container_size.width - child_size.width, .y = cy };
    case Alignment::BottomLeft: return Point{ .x = 0.0f, .y = container_size.height - child_size.height };
    case Alignment::BottomCenter: return Point{ .x = cx, .y = container_size.height - child_size.height };
    case Alignment::BottomRight:
        return Point{ .x = container_size.width - child_size.width, .y = container_size.height - child_size.height };
    }
    return Point{ .x = 0.0f, .y = 0.0f };
}

} // namespace aurora
