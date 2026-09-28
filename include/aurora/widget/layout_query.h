#pragma once

#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 布局查询。
///
/// 返回某节点已 layout/paint 后的布局结果（需 bounds 已填充）。
///
/// @param node 待查询的节点。
/// @return 节点当前 bounds 矩形；未布局时为默认值。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
///
[[nodiscard]] inline auto layout_of(const Node &node) -> Rect { return node.bounds(); }

/// @brief 返回某节点布局结果的结构化描述（JSON 行）。
/// @param node 待描述的节点。
/// @return 含 type/x/y/width/height 字段的 JSON 对象，取自节点 bounds 与 type_name。
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

}  // namespace aurora
