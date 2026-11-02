#pragma once

namespace aurora {

/// @brief 控件基准度量：一套设计语言里「控件该有多高、线该有多粗」的那组数值，单位一律逻辑 dp。
///
/// `Theme` 的纯值字段（与 `ShapeScale` / `SpacingScale` 同层，不分明暗）。形状档位管圆角、
/// 间距档位管留白，本组管**控件自身的基准尺寸与描边厚度**——三层合起来才构成「换一套设计
/// 语言」的观感骨架，只换配色做不到。
///
/// 取值分级：
/// - 标「复现现值」者逐位等于令牌层落地前的硬编码，出处在字段注释里点名到具体符号；同一语义
///   在库里存在多个现值时，本组取**多数消费方**那一个，其余现值在接线批逐字段替换时各自对齐；
/// - 标「候选」者**没有**对应的既有实现可复现（该能力尚未落地，如滚动条绘制、命中热区下限），
///   落码时须逐值补官方 HIG / 上游规范出处。
///
/// 单位纪律：全部为逻辑 dp，消费方按既有 DPI 换算路径折算物理像素，本结构体不做任何 scale 运算。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct ControlMetrics {
    float control_height = 30.0F;  ///< 控件基准高（复现现值：`Dropdown` 框高与 `SpinBox` 盒高同为 30）。
    float control_height_small = 24.0F;  ///< 紧凑控件基准高（复现现值：`Switch` 轨高 24）。
    float min_touch_target = 40.0F;  ///< 命中盒下限（候选：库内无该常量；图标按钮点击区现按 24 + 8×2 自然得 40）。
    float menu_item_height = 26.0F;  ///< 菜单 / 列表项行高（复现现值：`MenuBar` 行高与 `Dropdown` 选项行同为 26）。
    float focus_ring_width = 2.0F;  ///< 焦点环宽（复现现值：`Widget` 的焦点环厚度常量）。
    float focus_ring_inset = 2.0F;  ///< 焦点环与控件边框的间距（复现现值：`Widget` 的环间距常量；正值 = 向外）。
    float indicator_size =
        20.0F;  ///< 勾选基准盒边长（复现现值：`Checkbox` 方框边长；单选外径现值 16，接线批各自对齐）。
    float scrollbar_thickness = 4.0F;  ///< 滚动条粗细（候选：库内尚未绘制滚动条，无既有实现可复现）。
    float separator_thickness = 1.0F;  ///< 分隔线厚度（复现现值：`Divider` 默认线厚，另有五处同值硬编码）。
    float border_thickness = 1.0F;  ///< 控件默认描边宽（复现现值：`TextInput` / `Dropdown` 描边宽；勾选框 1.5 单列）。
    float icon_default_size = 24.0F;  ///< 图标默认边长（复现现值：`Icon` / `IconButton` 盒边长）。
    float content_padding = 8.0F;  ///< 通用内容留白（复现现值：`IconButton` 内边距与四张图表留白同为 8）。

    /// @brief 逐字段相等比较（十二项全等）；供主题解析与测试断言。
    /// @param other 比较对象。
    /// @return 全部字段相等时为 true。
    [[nodiscard]] auto operator==(const ControlMetrics &other) const -> bool = default;
};

}  // namespace aurora
