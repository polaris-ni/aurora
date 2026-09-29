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

/// @brief Aurora UI 库顶层命名空间（本头承载 Column / Row 线性容器与其布局上下文 trampoline）。
namespace aurora {

/// @brief 容器布局上下文：打包 children 指针 + 索引 + BuildContext 指针，供 trampoline 解包。
struct ContainerLayoutCtx : LayoutCtxBase {
    Node *children{};  ///< 子节点数组首元素
    size_t index = 0;  ///< 当前子项索引
    const BuildContext *build_ctx{};  ///< 构建上下文
};

/// @brief ContainerLayoutCtx 的 trampoline：void* → 具体类型 → 调用 widget.layout。
/// @param ctx 指向 `ContainerLayoutCtx` 的不透明指针（trampoline 约定；实际指向 ctxs 数组中该子项的上下文）。
/// @param c 布局器为该子项分配的约束。
/// @return 子控件 `layout()` 在给定约束下返回的尺寸。
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
/// @param ctx 指向 `ContainerLayoutCtx` 的不透明指针（trampoline 约定）。
/// @param measured 布局器传入的子项测量尺寸（用于查询 modifier 的内容盒平移量）。
/// @return 「布局盒顶 → 首行基线」距离；子控件无基线时为 `nullopt`（布局器走合成基线）。
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
    std::vector<Node> children;  ///< 子节点数组（构造时移入容器的 children_）
    Flex flex{.direction = FlexDirection::Column};  ///< flex 参数：主轴/交叉轴对齐（默认纵向）。
    float gap = 0.0F;  ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
};

/// @brief 纵向线性布局容器（主轴 = 垂直）。
///
/// 通过 `FlexLayouter` 完成两阶段布局：子项按 flex 权重瓜分剩余高度，交叉轴取最宽子项；
/// 主轴/交叉轴对齐与 `Expand`（经 `Modifier::expand`）见 specification/03-layout-render.md §7.2。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Column : public Container, public ColumnProps {
  public:
    /// @brief 默认构造：无子项、默认纵向 flex 配置与 gap=0。
    Column() = default;
    /// @brief 由属性聚合构造：子项移入 children_，flex 轴向强制为纵向（保留 Reverse 取值）。
    /// @param props 列属性聚合（子项、flex 对齐、间距）。
    explicit Column(ColumnProps props) {
        children_ = std::move(props.children);
        flex = props.flex;
        // 轴向归属控件类型：部分指定的 `Flex{.main_axis = X}` 会把 direction 打回默认值 Row，令 Column 横向排布。
        // 只纠轴向、保留 Reverse 取值（反向布局的既有表达入口，见 specification/03-layout-render.md §3.7）。
        flex.direction =
            props.flex.direction == FlexDirection::RowReverse ? FlexDirection::ColumnReverse : FlexDirection::Column;
        gap = props.gap;
    }
    /// @brief 便捷构造：扁平罗列子项（Column{ a, b }），免写 Node{} 与 Props 包裹。
    /// @param kids 初始子项列表（经 set_children 存入容器）。
    Column(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 设置相邻子项间距（链式）：`Column{...}.gap(12)`。
    /// @param g 间距（dp；负值由 validate_props 拦截回退默认）。
    /// @return Column 引用（链式调用）。
    auto set_gap(float g) -> Column & {
        gap = g;
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    /// @param a 主轴对齐策略。
    /// @return *this 引用（链式调用）。
    auto set_main_axis_alignment(MainAxisAlignment a) -> Column & {
        flex.main_axis = a;
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    /// @param a 交叉轴对齐策略。
    /// @return *this 引用（链式调用）。
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Column & {
        flex.cross_axis = a;
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    /// @param s 主轴尺寸策略（Min 贴合内容 / Max 撑满约束）。
    /// @return *this 引用（链式调用）。
    auto set_main_axis_size(MainAxisSize s) -> Column & {
        flex.main_axis_size = s;
        return *this;
    }

    /// @brief 序列化主轴/交叉轴对齐、主轴尺寸策略与 gap 到属性 JSON（先经基类写公共属性）。
    /// @param props 写入目标 JSON 对象（就地填充键值）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("main_axis_alignment", main_axis_alignment_to_json(flex.main_axis));
        props.set("cross_axis_alignment", cross_axis_alignment_to_json(flex.cross_axis));
        props.set("main_axis_size", main_axis_size_to_json(flex.main_axis_size));
        props.set("gap", gap);
    }

    /// @brief 从属性 JSON 恢复对齐/尺寸策略/gap（各键可选；gap 经 PropDescriptor 校验，非法回退 0）。
    /// @param props 读取来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("main_axis_alignment")) {
            flex.main_axis = json_to_main_axis_alignment(*props.at("main_axis_alignment"));
        }
        if (props.contains("cross_axis_alignment")) {
            flex.cross_axis = json_to_cross_axis_alignment(*props.at("cross_axis_alignment"));
        }
        if (props.contains("main_axis_size")) {
            flex.main_axis_size = json_to_main_axis_size(*props.at("main_axis_size"));
        }
        if (props.contains("gap")) {
            static const PropDescriptor D_GAP{.name = "gap", .json_type = "number", .min_value = "0"};
            gap = validate_or_default<float>(*props.at("gap"), D_GAP, 0.0F);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    /// @return gap 为负时返回 WidgetInvalidProp 错误；否则返回成功。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "Column"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "Column"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return WidgetDescriptor 静态描述表（名称/属性/事件/不变量/示例）。
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
    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（名称/属性/不变量/示例）。
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
        std::vector<ContainerLayoutCtx> ctxs(children_.size());  // 每个子项一个布局上下文槽（与 children_ 等长）
        std::vector<FlexItem> items;  // 传给 FlexLayouter 的子项度量视图
        items.reserve(children_.size());  // 预置与子项数等量容量，避免逐项扩容
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier.get().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg = flex;  // 本帧生效 flex 配置的副本（gap 与 rtl 在副本上覆写，不改成员）
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
    std::vector<Node> children;  ///< 子节点数组（构造时移入容器的 children_）
    Flex flex{.direction = FlexDirection::Row};  ///< flex 参数：主轴/交叉轴对齐（默认横向）。
    float gap = 0.0F;  ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
};

/// @brief 横向线性布局容器（主轴 = 水平）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Row : public Container, public RowProps {
  public:
    /// @brief 默认构造：无子项、默认横向 flex 配置与 gap=0。
    Row() = default;
    /// @brief 由属性聚合构造：子项移入 children_，flex 轴向强制为横向（保留 Reverse 取值）。
    /// @param props 行属性聚合（子项、flex 对齐、间距）。
    explicit Row(RowProps props) {
        children_ = std::move(props.children);
        flex = props.flex;
        // 与 Column 对称：轴向归属控件类型，Reverse 取值保留。
        flex.direction =
            props.flex.direction == FlexDirection::ColumnReverse ? FlexDirection::RowReverse : FlexDirection::Row;
        gap = props.gap;
    }
    /// @brief 便捷构造：扁平罗列子项（Row{ a, b }），免写 Node{} 与 Props 包裹。
    /// @param kids 初始子项列表（经 set_children 存入容器）。
    Row(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 设置相邻子项间距（链式）：`Row{...}.gap(12)`。
    /// @param g 间距（dp；负值由 validate_props 拦截回退默认）。
    /// @return Row 引用（链式调用）。
    auto set_gap(float g) -> Row & {
        gap = g;
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    /// @param a 主轴对齐策略。
    /// @return *this 引用（链式调用）。
    auto set_main_axis_alignment(MainAxisAlignment a) -> Row & {
        flex.main_axis = a;
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    /// @param a 交叉轴对齐策略。
    /// @return *this 引用（链式调用）。
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Row & {
        flex.cross_axis = a;
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    /// @param s 主轴尺寸策略（Min 贴合内容 / Max 撑满约束）。
    /// @return *this 引用（链式调用）。
    auto set_main_axis_size(MainAxisSize s) -> Row & {
        flex.main_axis_size = s;
        return *this;
    }

    /// @brief 序列化主轴/交叉轴对齐、主轴尺寸策略与 gap 到属性 JSON（先经基类写公共属性）。
    /// @param props 写入目标 JSON 对象（就地填充键值）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("main_axis_alignment", main_axis_alignment_to_json(flex.main_axis));
        props.set("cross_axis_alignment", cross_axis_alignment_to_json(flex.cross_axis));
        props.set("main_axis_size", main_axis_size_to_json(flex.main_axis_size));
        props.set("gap", gap);
    }

    /// @brief 从属性 JSON 恢复对齐/尺寸策略/gap（各键可选；gap 经 PropDescriptor 校验，非法回退 0）。
    /// @param props 读取来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("main_axis_alignment")) {
            flex.main_axis = json_to_main_axis_alignment(*props.at("main_axis_alignment"));
        }
        if (props.contains("cross_axis_alignment")) {
            flex.cross_axis = json_to_cross_axis_alignment(*props.at("cross_axis_alignment"));
        }
        if (props.contains("main_axis_size")) {
            flex.main_axis_size = json_to_main_axis_size(*props.at("main_axis_size"));
        }
        if (props.contains("gap")) {
            static const PropDescriptor D_GAP{.name = "gap", .json_type = "number", .min_value = "0"};
            gap = validate_or_default<float>(*props.at("gap"), D_GAP, 0.0F);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    /// @return gap 为负时返回 WidgetInvalidProp 错误；否则返回成功。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "Row"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "Row"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return WidgetDescriptor 静态描述表（名称/属性/事件/不变量/示例）。
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
    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（名称/属性/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        std::vector<ContainerLayoutCtx> ctxs(children_.size());  // 每个子项一个布局上下文槽（与 children_ 等长）
        std::vector<FlexItem> items;  // 传给 FlexLayouter 的子项度量视图
        items.reserve(children_.size());  // 预置与子项数等量容量，避免逐项扩容
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier.get().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg = flex;  // 本帧生效 flex 配置的副本（gap 与 rtl 在副本上覆写，不改成员）
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
