#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "aurora/core/enums.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"

namespace aurora {

class Painter;  // 前向声明；实际绘制在 widget 模块按节点种类解释

/// @brief 修饰节点基类（布局 / 绘制 / 输入 正交切片）。
///
/// 参考 Compose `Modifier` / SwiftUI 修饰符链：把 padding/background/clickable 等
/// 跨切面关注点抽离出各 widget， compositional 地叠加在 `WidgetProps.modifier` 上。
///
/// 为保持 `modifier` 模块零依赖（不引入 render/widget），绘制逻辑由 widget 模块
/// 的 `Widget::paint` 按节点 `kind()` 解释执行（见 widget/widget.hpp）。
///
/// 子类按功能切片分布于独立头文件：
/// - `modifier_layout.h`：Padding / PaddingEdges / FlexWeight / SizeModifier
/// - `modifier_transform.h`：AlignNode / OffsetNode / TransformNode
/// - `modifier_paint.h`：Background / GradientBackground / ShadowNode / BlendNode /
/// ShaderMaskNode / CacheLayerNode / Border / Clip / ClipRounded / OpacityNode / BlurNode
/// - `modifier_input.h`：Clickable / Draggable / LongPress / TouchListener / TooltipNode / ContextMenuNode
///
/// 聚合入口 `modifier.h` 包含全部切片并提供 `Modifier` 工厂链。
class ModifierNode {
  public:
    enum class Kind : std::uint8_t {
        Layout,  ///< 影响测量（如 Padding）
        Paint,  ///< 影响绘制（如 Background）
        Input,  ///< 影响输入（如 Clickable）
        Transform,  ///< 影响绘制期平移/对齐（如 Align / Offset）
    };

    /// @brief Paint 切片细分子类型（供 switch 分发替代 dynamic_cast）。
    enum class PaintKind : std::uint8_t {
        None,  ///< 非 Paint 节点（默认）
        Background,  ///< Background
        GradientBackground,  ///< GradientBackground
        Shadow,  ///< ShadowNode
        Blend,  ///< BlendNode
        ShaderMask,  ///< ShaderMaskNode
        CacheLayer,  ///< CacheLayerNode
        Border,  ///< Border
        Clip,  ///< Clip
        ClipRounded,  ///< ClipRounded
        Blur,  ///< BlurNode
    };

    ModifierNode() = default;
    ModifierNode(const ModifierNode &) = default;
    ModifierNode(ModifierNode &&) = default;
    auto operator=(const ModifierNode &) -> ModifierNode & = default;
    auto operator=(ModifierNode &&) -> ModifierNode & = default;
    virtual ~ModifierNode() = default;

    /// @brief 节点类别鉴别（正交切片归属）。
    /// @return 本节点所属切片：Layout / Paint / Input / Transform。
    [[nodiscard]] virtual auto kind() const -> Kind = 0;

    /// @brief Paint 子类型鉴别（仅 Kind::Paint 节点覆盖；其余返回 None）。
    /// @return Paint 切片内的具体子类；基类默认 PaintKind::None（非 Paint 节点）。
    [[nodiscard]] virtual auto paint_kind() const -> PaintKind { return PaintKind::None; }

    /// @brief 布局：接收约束与「测量子节点」回调，返回本节点尺寸（外→内包裹）。
    /// @param c 本节点收到的外部约束。
    /// @param measure_child 以给定约束测量子节点的回调（子类改写约束后传入）。
    /// @return 本节点的内容尺寸（通常为子节点测量结果按切片语义加减后的值）。
    virtual auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size = 0;

    /// @brief Flex 权重（仅 `FlexWeight` 节点覆盖；其余节点返回 0 = 不扩展）。
    /// 由 Row/Column 在 flex 布局时读取，用于瓜分主轴剩余空间（对应 Expand）。
    /// @return flex 权重；非 FlexWeight 节点恒为 0.0F（仅占内容尺寸）。
    [[nodiscard]] virtual auto flex_weight() const -> float { return 0.0F; }

    /// @brief 悬停光标形状（仅 `CursorNode` 覆盖；其余节点返回空 = 不声明）。
    /// 由事件派发器在悬停链变化时读取，解析出目标形状后经 `Surface::set_cursor` 下发。
    /// @return 声明的光标形状；非 CursorNode 节点恒为 `std::nullopt`。
    [[nodiscard]] virtual auto cursor_shape() const -> std::optional<CursorShape> { return std::nullopt; }

    /// @brief 触发点击回调（仅 `Clickable` 节点覆盖；其余节点为空操作）。
    /// 由事件派发器在命中目标上调用（架构 §7 事件与命中测试）。
    virtual auto fire_click() const -> void {}

    /// @brief 原始触摸事件入口（仅 `TouchListener` 等输入节点覆盖；其余节点为空操作）。
    /// 由 `Widget::on_pointer_event(TouchEvent&)` 在命中链上调用，交付完整 `TouchEvent`（多点原始流）。
    virtual auto on_touch(const TouchEvent & /*e*/) const -> void {}
};

}  // namespace aurora
