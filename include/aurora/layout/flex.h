#pragma once

#include "aurora/core/enums.h"

namespace aurora {

/**
 * @brief 弹性布局方向（对应架构 §4.2 / Flutter Flex）。
 */
enum class FlexDirection : std::uint8_t {
    Row,
    Column,
    RowReverse,
    ColumnReverse,
};

/**
 * @brief 弹性布局参数：挂载在 Row/Column 上即可启用 Flutter 式 flex 布局。
 *
 * 与 widget 的 modifier 正交：本结构描述"子项如何排布"，modifier 描述"自身如何修饰"。
 */
struct Flex {
    FlexDirection direction = FlexDirection::Row;
    MainAxisAlignment main_axis = MainAxisAlignment::Start;
    CrossAxisAlignment cross_axis = CrossAxisAlignment::Start;
    /// @brief 相邻子项间的固定间距（像素）。等价于 Flutter 的 `spacing`：
    /// 无论主轴对齐方式如何，相邻子项间至少插入 `gap`；剩余空间仍由 `main_axis` 分配。
    /// 默认 0（不插入额外间距）。
    float gap = 0.0F;
    /// @brief 主轴尺寸策略（对应 Flutter `MainAxisSize`）。
    /// - `Min`（默认）：容器主轴取"内容所需尺寸"，主轴对齐仅当父约束强制更大尺寸时才有可见自由空间。
    /// - `Max`：容器主轴撑满父级可用主轴空间，从而让 `main_axis` 对齐在内容不足时产生可见自由空间。
    MainAxisSize main_axis_size = MainAxisSize::Min;
    /// @brief 书写方向镜像（A2）：RTL 时全部子项在容器内容宽度内做水平镜像——
    /// 水平主轴（Row/RowReverse）子项视觉顺序翻转且 `Start/End` 语义对调；
    /// 垂直主轴（Column/ColumnReverse）交叉轴（水平）`Start/End` 对调；
    /// `Center`/`Space*` 对称结果不变。由 Row/Column 按 `Directionality` 注入（见
    /// core/directionality.h）；FlexLayouter 单测可直接置位验证几何。
    bool rtl = false;
};

}  // namespace aurora
