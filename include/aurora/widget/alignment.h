#pragma once

#include <cstdint>

#include "aurora/core/types.h"

namespace aurora {

/// @brief 对齐方式（九宫格）：控制子项在可用空间内的定位。
///
/// 单独成文件以避免 `modifier.h` ↔ `widget/stack.h` 的循环包含
/// （`modifier` 需 `Alignment` 实现 `align()`，而 `stack.h` 也产出 `Stack`）。
///
/// @note Thread: thread-safe (pure enum)
/// @note Side-effects: none
/// @note Rebuildable: no
///
enum class Alignment : std::uint8_t {
    TopLeft,  ///< 左上角对齐：子项贴容器左上角。
    TopCenter,  ///< 顶部水平居中：X 取容器中部余量，Y 贴顶。
    TopRight,  ///< 右上角对齐：X 取容器右缘减子项宽，Y 贴顶。
    CenterLeft,  ///< 左侧垂直居中：X 贴左，Y 取容器中部余量。
    Center,  ///< 完全居中：X/Y 均取容器中部余量。
    CenterRight,  ///< 右侧垂直居中：X 取容器右缘减子项宽，Y 取中部余量。
    BottomLeft,  ///< 左下角对齐：X 贴左，Y 取容器底缘减子项高。
    BottomCenter,  ///< 底部水平居中：X 取中部余量，Y 取容器底缘减子项高。
    BottomRight  ///< 右下角对齐：X/Y 均取容器缘减子项尺寸。
};

/// @brief 返回子项相对容器的对齐原点（左上角），使子项按 `a` 定位。
/// @param a 九宫格对齐方式。
/// @param child_size 子项尺寸。
/// @param container_size 容器可用空间尺寸。
/// @return 子项左上角在容器坐标系中的位置；未匹配时回退到左上角 (0, 0)。
inline auto align_origin(Alignment a, Size child_size, Size container_size) -> Point {
    const float cx = (container_size.width - child_size.width) * 0.5F;
    const float cy = (container_size.height - child_size.height) * 0.5F;
    switch (a) {
        case Alignment::TopLeft:
            return Point{.x = 0.0F, .y = 0.0F};
        case Alignment::TopCenter:
            return Point{.x = cx, .y = 0.0F};
        case Alignment::TopRight:
            return Point{.x = container_size.width - child_size.width, .y = 0.0F};
        case Alignment::CenterLeft:
            return Point{.x = 0.0F, .y = cy};
        case Alignment::Center:
            return Point{.x = cx, .y = cy};
        case Alignment::CenterRight:
            return Point{.x = container_size.width - child_size.width, .y = cy};
        case Alignment::BottomLeft:
            return Point{.x = 0.0F, .y = container_size.height - child_size.height};
        case Alignment::BottomCenter:
            return Point{.x = cx, .y = container_size.height - child_size.height};
        case Alignment::BottomRight:
            return Point{.x = container_size.width - child_size.width, .y = container_size.height - child_size.height};
    }
    return Point{.x = 0.0F, .y = 0.0F};
}

}  // namespace aurora
