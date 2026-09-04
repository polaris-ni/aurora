#pragma once

#include <vector>

#include "aurora/core/types.h"

namespace aurora {

/**
 * @brief 布局盒：一次布局 pass 后某 widget 的几何结果（位置 + 尺寸 + 约束 + 子盒）。
 *
 * 与 Widget 实例分离，便于命中测试、调试快照与无头渲染复用，不干扰 widget 树本身。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct LayoutBox {
    Rect rect{};  ///< 相对父节点的坐标与尺寸
    Constraints constraints{};  ///< 本节点收到的约束
    std::vector<LayoutBox> children;  ///< 子节点布局盒（顺序与 widget 子节点一致）
};

}  // namespace aurora
