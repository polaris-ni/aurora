#pragma once

#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 布局查询（规格 §2.4）。
 *
 * 返回某节点已 layout/paint 后的布局结果（需 bounds 已填充）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto layout_of(const Node &node) -> Rect { return node.bounds(); }

/// @brief 返回某节点布局结果的结构化描述（JSON 行）。
[[nodiscard]] inline auto describe_layout(const Node &node) -> Json {
    Json j = Json::object();
    const Rect b = node.bounds();
    j["type"] = node.widget().type_name();
    j["x"] = b.origin.x;
    j["y"] = b.origin.y;
    j["width"] = b.size.width;
    j["height"] = b.size.height;
    return j;
}

} // namespace aurora
