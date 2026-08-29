#pragma once

#include "aurora/core/types.h"
#include "aurora/window/title_bar_style.h"

namespace aurora {

/// @brief 标题栏各分区矩形（窗口逻辑坐标，原点=窗口左上角）。
/// 由 `title_bar_geometry()` 计算，绘制层与命中测试层共用同一份结果（单一权威来源，
/// 两层不得各自推几何，避免绘制热区与命中热区漂移）。
/// 隐藏分区的约定表示法：空盒 `Size{0,0}`（以 `width == 0` 判空）。
struct TitleBarGeometry {
    Rect close{};    ///< 关闭按钮矩形（隐藏时为空盒 width==0）
    Rect maximize{}; ///< 最大化按钮矩形（!resizable 或 show_maximize=false 时为空盒）
    Rect minimize{}; ///< 最小化按钮矩形（show_minimize=false 时为空盒）
    Rect icon{};     ///< 图标槽（本函数不感知图标显隐开关，恒返回计算位；是否绘制由上层决定）
    Rect title{};    ///< 标题文字可用区（垂直全高，文字由绘制层居中；窄窗挤压退化时为空盒）
};

/// @brief 计算标题栏几何（纯函数）。width=窗口宽；style 提供高度与布局；maximized
/// 决定最大化/还原图标字形（不影响按钮盒尺寸，由绘制层消费）；resizable=false 时
/// maximize 盒为空。Mac 布局按钮在左侧，其余在右侧。
/// 尺寸规则唯一权威来源见实现文件顶部注释块。
[[nodiscard]] auto title_bar_geometry(float width, const TitleBarStyle &style, bool maximized, bool resizable)
    -> TitleBarGeometry;

} // namespace aurora
