#pragma once

#include <initializer_list>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/directionality.h"
#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 容器布局上下文：打包 children 指针 + 索引 + BuildContext 指针，供 trampoline 解包。
struct ContainerLayoutCtx : LayoutCtxBase {
    Node *children{};  ///< 子节点数组首元素
    size_t index = 0;  ///< 当前子项索引
    const BuildContext *build_ctx{};  ///< 构建上下文
};

/// @brief ContainerLayoutCtx 的 trampoline：void* → 具体类型 → 调用 widget.layout。
inline auto container_measure(void *ctx, const Constraints &c) -> Size {
    const auto *lc = static_cast<ContainerLayoutCtx *>(ctx);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) trampoline 用裸指针+索引解包子节点
    return lc->children[lc->index].widget().layout(c, *lc->build_ctx);
}

/// @brief ContainerLayoutCtx 的基线 trampoline（`FlexItem::BaselineFn`）：
///        解包子控件 → 取控件级基线 → 补上内容盒位移，得到「布局盒顶 → 基线」。
///
/// 语义链：`Widget::baseline_distance(ctx)` 给出「**内容盒**顶 → 首行基线」（含控件自身内边距），
/// 而布局器需要的是「**布局盒**顶 → 基线」，两者相差 `Modifier::transform(measured).translation`
/// 的 y 分量（`Modifier::padding` / Align 造成的内容盒平移）。子控件无基线时返回 `nullopt`，
/// 布局器对之走 CSS 式合成基线（交叉轴底边），不报错。
///
/// `measured` 由布局器传入（测量期的子项尺寸），因此本函数**不**依赖 `Node::bounds`——
/// 布局器是在 `Row/Column::on_layout` 的 `set_bounds` 之前被调用的。
inline auto container_baseline(void *ctx, Size measured) -> std::optional<float> {
    const auto *lc = static_cast<ContainerLayoutCtx *>(ctx);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) trampoline 用裸指针+索引解包子节点
    const Widget &child = lc->children[lc->index].widget();
    const std::optional<float> baseline = child.baseline_distance(*lc->build_ctx);
    if (!baseline.has_value()) {
        return std::nullopt;
    }
    return *baseline + child.modifier.get().transform(measured).translation.y;
}

/// @brief Column 属性（聚合）。
struct ColumnProps {
    std::vector<Node> children;
    Flex flex{.direction = FlexDirection::Column};  ///< flex 参数：主轴/交叉轴对齐（默认纵向）。
    float gap = 0.0F;  ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
};

/**
 * @brief 纵向线性布局容器（主轴 = 垂直）。
 *
 * 通过 `FlexLayouter` 完成两阶段布局：子项按 flex 权重瓜分剩余高度，交叉轴取最宽子项；
 * 主轴/交叉轴对齐与 `Expand`（经 `Modifier::expand`）见 specification/03-layout-render.md §7.2。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Column : public Container, public ColumnProps {
  public:
    Column() = default;
    explicit Column(ColumnProps props) {
        children_ = std::move(props.children);
        flex = props.flex;
        gap = props.gap;
    }
    /// @brief 便捷构造：扁平罗列子项（Column{ a, b }），免写 Node{} 与 Props 包裹。
    Column(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 设置相邻子项间距（链式）：`Column{...}.gap(12)`。
    auto set_gap(float g) -> Column & {
        gap = g;
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    auto set_main_axis_alignment(MainAxisAlignment a) -> Column & {
        flex.main_axis = a;
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Column & {
        flex.cross_axis = a;
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    auto set_main_axis_size(MainAxisSize s) -> Column & {
        flex.main_axis_size = s;
        return *this;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["main_axis_alignment"] = main_axis_alignment_to_json(flex.main_axis);
        props["cross_axis_alignment"] = cross_axis_alignment_to_json(flex.cross_axis);
        props["main_axis_size"] = main_axis_size_to_json(flex.main_axis_size);
        props["gap"] = gap;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("main_axis_alignment")) {
            flex.main_axis = json_to_main_axis_alignment(props["main_axis_alignment"]);
        }
        if (props.contains("cross_axis_alignment")) {
            flex.cross_axis = json_to_cross_axis_alignment(props["cross_axis_alignment"]);
        }
        if (props.contains("main_axis_size")) {
            flex.main_axis_size = json_to_main_axis_size(props["main_axis_size"]);
        }
        if (props.contains("gap")) {
            static const PropDescriptor D_GAP{.name = "gap", .json_type = "number", .min_value = "0"};
            gap = validate_or_default<float>(props["gap"], D_GAP, 0.0F);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Column"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Column",
            .properties =
                {
                    {.name = "main_axis_alignment",
                     .type = "MainAxisAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "主轴对齐",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"}},
                    {.name = "cross_axis_alignment",
                     .type = "CrossAxisAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "交叉轴对齐",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End", "Stretch", "Baseline"}},
                    {.name = "main_axis_size",
                     .type = "MainAxisSize",
                     .default_value = "Min",
                     .required = false,
                     .note = "主轴尺寸策略",
                     .json_type = "string",
                     .enum_values = {"Min", "Max"}},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "子项间距(px)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .invariants = {"gap >= 0"},
            .examples = {R"(au::Column{ au::Text("A"), au::Text("B") })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // Column 的交叉轴是水平的：`CrossAxisAlignment::Baseline` 没有基线语义，布局器按 `Start` 处理。
        // 一次性降级提示（每实例仅一次，不每帧刷屏）——放在布局入口可同时覆盖 setter / from_json /
        // 直写 `flex.cross_axis` 三条路径，无需在每处状态变更点重复接线。
        if (flex.cross_axis == CrossAxisAlignment::Baseline && !baseline_degraded_warned_) {
            baseline_degraded_warned_ = true;
            Diagnostics::degraded("Column 的交叉轴是水平的，CrossAxisAlignment::Baseline 无基线语义，已按 Start 处理",
                                  "layout");
        }
        std::vector<ContainerLayoutCtx> ctxs(children_.size());
        std::vector<FlexItem> items;
        items.reserve(children_.size());
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier.get().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg = flex;
        cfg.gap = gap > 0.0F ? gap : flex.gap;
        // 布局镜像：按生效书写方向（Environment/进程级）翻转水平排布。
        cfg.rtl = resolved_text_direction(ctx) == TextDirection::RTL;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < children_.size(); ++i) {
            children_[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }

  private:
    bool baseline_degraded_warned_ = false;  ///< `Baseline` 降级提示是否已发（每实例一次，避免逐帧刷屏）
};

/// @brief Row 属性（聚合）。
struct RowProps {
    std::vector<Node> children;
    Flex flex{.direction = FlexDirection::Row};  ///< flex 参数：主轴/交叉轴对齐（默认横向）。
    float gap = 0.0F;  ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
};

/**
 * @brief 横向线性布局容器（主轴 = 水平）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Row : public Container, public RowProps {
  public:
    Row() = default;
    explicit Row(RowProps props) {
        children_ = std::move(props.children);
        flex = props.flex;
        gap = props.gap;
    }
    /// @brief 便捷构造：扁平罗列子项（Row{ a, b }），免写 Node{} 与 Props 包裹。
    Row(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 设置相邻子项间距（链式）：`Row{...}.gap(12)`。
    auto set_gap(float g) -> Row & {
        gap = g;
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    auto set_main_axis_alignment(MainAxisAlignment a) -> Row & {
        flex.main_axis = a;
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Row & {
        flex.cross_axis = a;
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    auto set_main_axis_size(MainAxisSize s) -> Row & {
        flex.main_axis_size = s;
        return *this;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["main_axis_alignment"] = main_axis_alignment_to_json(flex.main_axis);
        props["cross_axis_alignment"] = cross_axis_alignment_to_json(flex.cross_axis);
        props["main_axis_size"] = main_axis_size_to_json(flex.main_axis_size);
        props["gap"] = gap;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("main_axis_alignment")) {
            flex.main_axis = json_to_main_axis_alignment(props["main_axis_alignment"]);
        }
        if (props.contains("cross_axis_alignment")) {
            flex.cross_axis = json_to_cross_axis_alignment(props["cross_axis_alignment"]);
        }
        if (props.contains("main_axis_size")) {
            flex.main_axis_size = json_to_main_axis_size(props["main_axis_size"]);
        }
        if (props.contains("gap")) {
            static const PropDescriptor D_GAP{.name = "gap", .json_type = "number", .min_value = "0"};
            gap = validate_or_default<float>(props["gap"], D_GAP, 0.0F);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Row"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Row",
            .properties =
                {
                    {.name = "main_axis_alignment",
                     .type = "MainAxisAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "主轴对齐",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"}},
                    {.name = "cross_axis_alignment",
                     .type = "CrossAxisAlignment",
                     .default_value = "Start",
                     .required = false,
                     .note = "交叉轴对齐",
                     .json_type = "string",
                     .enum_values = {"Start", "Center", "End", "Stretch", "Baseline"}},
                    {.name = "main_axis_size",
                     .type = "MainAxisSize",
                     .default_value = "Min",
                     .required = false,
                     .note = "主轴尺寸策略",
                     .json_type = "string",
                     .enum_values = {"Min", "Max"}},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "子项间距(px)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .invariants = {"gap >= 0"},
            .examples = {R"(au::Row{ au::Text("A"), au::Text("B") })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        std::vector<ContainerLayoutCtx> ctxs(children_.size());
        std::vector<FlexItem> items;
        items.reserve(children_.size());
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier.get().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg = flex;
        cfg.gap = gap > 0.0F ? gap : flex.gap;
        // 布局镜像：按生效书写方向（Environment/进程级）翻转水平排布。
        cfg.rtl = resolved_text_direction(ctx) == TextDirection::RTL;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < children_.size(); ++i) {
            children_[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }
};

}  // namespace aurora
