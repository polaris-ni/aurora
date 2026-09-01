#pragma once

#include <initializer_list>
#include <vector>

#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 容器布局上下文：打包 children 指针 + 索引 + BuildContext 指针，供 trampoline 解包。
struct ContainerLayoutCtx : LayoutCtxBase {
    Node *children{};                ///< 子节点数组首元素
    size_t index = 0;                ///< 当前子项索引
    const BuildContext *build_ctx{}; ///< 构建上下文
};

/// @brief ContainerLayoutCtx 的 trampoline：void* → 具体类型 → 调用 widget.layout。
inline auto container_measure(void *ctx, const Constraints &c) -> Size {
    const auto *lc = static_cast<ContainerLayoutCtx *>(ctx);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) trampoline 用裸指针+索引解包子节点
    return lc->children[lc->index].widget().layout(c, *lc->build_ctx);
}

/// @brief Column 属性（聚合）。
struct ColumnProps {
    std::vector<Node> children;
    Flex flex{ .direction = FlexDirection::Column }; ///< flex 参数：主轴/交叉轴对齐（默认纵向）。
    float gap = 0.0f;                                ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
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
        m_children = std::move(props.children);
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
            static const PropDescriptor d_gap{ .name = "gap", .json_type = "number", .min_value = "0" };
            gap = validate_or_default<float>(props["gap"], d_gap, 0.0f);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0f) {
            return make_error(ErrorCode::WidgetInvalidProp, "布局 gap 必须 >= 0，得到 " + std::to_string(gap),
                              "使用非负间距");
        }
        return Result<void>{};
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Column"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Column",
            .properties = {
                { .name="main_axis_alignment", .type="MainAxisAlignment", .default_value="Start", .required=false, .note="主轴对齐", .json_type="string",
                  .enum_values={"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"} },
                { .name="cross_axis_alignment", .type="CrossAxisAlignment", .default_value="Start", .required=false, .note="交叉轴对齐", .json_type="string",
                  .enum_values={"Start", "Center", "End", "Stretch"} },
                { .name="main_axis_size", .type="MainAxisSize", .default_value="Min", .required=false, .note="主轴尺寸策略", .json_type="string",
                  .enum_values={"Min", "Max"} },
                { .name="gap", .type="float", .default_value="0.0", .required=false, .note="子项间距(px)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="width", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="height", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="show", .type="bool", .default_value="true", .required=false, .note="", .json_type="boolean" },
            },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .invariants = { "gap >= 0" },
            .examples = { R"(au::Column{ au::Text("A"), au::Text("B") })" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        std::vector<ContainerLayoutCtx> ctxs(m_children.size());
        std::vector<FlexItem> items;
        items.reserve(m_children.size());
        for (size_t i = 0; i < m_children.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{ {}, m_children.data(), i, &ctx };
            const float w = m_children[i].widget().modifier.get().flex_weight();
            items.push_back(FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure));
        }
        Flex cfg = flex;
        cfg.gap = gap > 0.0f ? gap : flex.gap;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < m_children.size(); ++i) {
            m_children[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }
};

/// @brief Row 属性（聚合）。
struct RowProps {
    std::vector<Node> children;
    Flex flex{ .direction = FlexDirection::Row }; ///< flex 参数：主轴/交叉轴对齐（默认横向）。
    float gap = 0.0f;                             ///< 相邻子项固定间距（像素）；>0 时覆盖 `flex.gap`。
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
        m_children = std::move(props.children);
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
            static const PropDescriptor d_gap{ .name = "gap", .json_type = "number", .min_value = "0" };
            gap = validate_or_default<float>(props["gap"], d_gap, 0.0f);
        }
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap < 0.0f) {
            return make_error(ErrorCode::WidgetInvalidProp, "布局 gap 必须 >= 0，得到 " + std::to_string(gap),
                              "使用非负间距");
        }
        return Result<void>{};
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Row"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Row",
            .properties = {
                { .name="main_axis_alignment", .type="MainAxisAlignment", .default_value="Start", .required=false, .note="主轴对齐", .json_type="string",
                  .enum_values={"Start", "Center", "End", "SpaceBetween", "SpaceAround", "SpaceEvenly"} },
                { .name="cross_axis_alignment", .type="CrossAxisAlignment", .default_value="Start", .required=false, .note="交叉轴对齐", .json_type="string",
                  .enum_values={"Start", "Center", "End", "Stretch"} },
                { .name="main_axis_size", .type="MainAxisSize", .default_value="Min", .required=false, .note="主轴尺寸策略", .json_type="string",
                  .enum_values={"Min", "Max"} },
                { .name="gap", .type="float", .default_value="0.0", .required=false, .note="子项间距(px)", .json_type="number", .enum_values={}, .min_value="0" },
                { .name="width", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="height", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
                { .name="show", .type="bool", .default_value="true", .required=false, .note="", .json_type="boolean" },
            },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .invariants = { "gap >= 0" },
            .examples = { R"(au::Row{ au::Text("A"), au::Text("B") })" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        std::vector<ContainerLayoutCtx> ctxs(m_children.size());
        std::vector<FlexItem> items;
        items.reserve(m_children.size());
        for (size_t i = 0; i < m_children.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{ {}, m_children.data(), i, &ctx };
            const float w = m_children[i].widget().modifier.get().flex_weight();
            items.push_back(FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure));
        }
        Flex cfg = flex;
        cfg.gap = gap > 0.0f ? gap : flex.gap;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < m_children.size(); ++i) {
            m_children[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }
};

} // namespace aurora
