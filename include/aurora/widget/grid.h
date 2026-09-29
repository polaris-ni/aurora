#pragma once

#include <algorithm>
#include <initializer_list>
#include <vector>

#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Grid 属性（聚合）：固定列数的网格布局容器。
struct GridProps {
    std::vector<Node> children;  ///< 子节点集合（构造/adopt 时接管）。
    int columns = 1;  ///< 列数（行数 = ceil(n / columns)）
    float gap = 4.0F;  ///< 单元格间距（像素）
};

/// @brief 网格布局容器：按行优先把子项排进固定列数的网格。
///
/// 每列宽 = 该列最宽子项自然宽；每行高 = 该行最高子项自然高。
/// 若父约束给定有限宽度，则每列宽均分该宽度（子项在列宽约束下测量）。
///
/// 采用**继承式双模 API**（specification/04-widget.md §2.5）：`GridProps` 字段即本控件公有字段，
/// `columns`/`gap` 可直接赋值或以配置块构造
/// `Grid{ GridProps{.children = ..., .columns = 2, .gap = 8} }`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Grid : public Container, public GridProps {
  public:
    /// @brief 默认构造：无子项，columns=1、gap=4。
    Grid() = default;
    /// @brief 配置块构造（specification/04-widget.md §2.5）：children 移动进容器，columns/gap 直取字段值。
    /// @param props GridProps 配置块，其 columns/gap 覆盖同名公有字段。
    explicit Grid(GridProps props) {
        children_ = std::move(props.children);
        columns = props.columns;
        gap = props.gap;
    }
    /// @brief 便捷构造：扁平罗列子项（Grid{ a, b, c, 2 }），columns/gap 取默认。
    /// @param kids 子节点列表，转交 set_children 接管。
    /// @param columns 列数；小于 1 时钳为 1。
    /// @param gap 单元格间距（像素）。
    Grid(std::initializer_list<Node> kids, int columns = 1, float gap = 4.0F) {
        set_children(kids);
        this->columns = columns > 0 ? columns : 1;
        this->gap = gap;
    }

    /// @brief 设置列数（链式）。
    /// @param c 新列数；小于 1 时钳为 1。
    /// @return 自身引用，便于链式调用。
    auto set_columns(int c) -> Grid & {
        columns = c > 0 ? c : 1;
        return *this;
    }
    /// @brief 设置单元格间距（链式）。
    /// @param g 新间距（像素）。
    /// @return 自身引用，便于链式调用。
    auto set_gap(float g) -> Grid & {
        gap = g;
        return *this;
    }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；Grid 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}
    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Grid"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Grid"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Grid 的控件描述符（columns/gap/尺寸属性、multiple 子策略与示例）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Grid",
            .properties =
                {
                    {.name = "columns",
                     .type = "int",
                     .default_value = "1",
                     .required = false,
                     .note = "Column count",
                     .json_type = "integer",
                     .enum_values = {},
                     .min_value = "1"},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "4.0",
                     .required = false,
                     .note = "Cell spacing (px)",
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
            .invariants = {"columns >= 1", "gap >= 0"},
            .examples = {R"(au::Grid({ au::Text("A"), au::Text("B") }, 2))"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Grid 的控件描述符（columns/gap/尺寸属性、multiple 子策略与示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化网格属性：通用属性之外写入 columns 与 gap。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("columns", columns);
        props.set("gap", gap);
    }
    /// @brief 反序列化网格属性：读回 columns（<1 钳为 1）与 gap。
    /// @param props 源 JSON 对象，缺键的字段保持当前值。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("columns")) {
            const int c = props.at("columns")->as_or<std::int32_t>(0);
            columns = c > 0 ? c : 1;
        }
        if (props.contains("gap")) {
            gap = props.at("gap")->as_or<float>(0.0F);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const int cols = columns > 0 ? columns : 1;
        const float g = gap;
        const size_t n = children_.size();
        const int rows = static_cast<int>((n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));

        const bool width_bounded = (c.max.width != Size::infinity().width);
        float cell_w = -1.0F;
        if (width_bounded) {
            cell_w = (c.max.width - (g * static_cast<float>(cols - 1))) / static_cast<float>(cols);
            cell_w = std::max(cell_w, 0.0F);
        }

        std::vector<Size> sizes(n);
        for (size_t i = 0; i < n; ++i) {
            Constraints cc;
            cc.min = Size{.width = 0.0F, .height = 0.0F};
            cc.max = (cell_w >= 0.0F) ? Size{.width = cell_w, .height = Size::infinity().height} : Size::infinity();
            sizes[i] = children_[i].widget().layout(cc, ctx);
        }

        std::vector col_w(cols, 0.0F);
        std::vector row_h(rows, 0.0F);
        for (size_t i = 0; i < n; ++i) {
            const int col = static_cast<int>(i % static_cast<size_t>(cols));
            const int row = static_cast<int>(i / static_cast<size_t>(cols));
            col_w[col] = std::max(col_w[col], sizes[i].width);
            row_h[row] = std::max(row_h[row], sizes[i].height);
        }

        float total_w = 0.0F;
        float total_h = 0.0F;
        for (const float w : col_w) {
            total_w += w;
        }
        for (const float h : row_h) {
            total_h += h;
        }
        total_w += g * static_cast<float>(cols - 1);
        total_h += g * static_cast<float>(rows - 1);

        float x = 0.0F;
        for (int col = 0; col < cols; ++col) {
            float y = 0.0F;
            for (int row = 0; row < rows; ++row) {
                const size_t idx = (static_cast<size_t>(row) * static_cast<size_t>(cols)) + static_cast<size_t>(col);
                if (idx < n) {
                    children_[idx].set_bounds(Rect{.origin = Point{.x = x, .y = y}, .size = sizes[idx]});
                }
                y += row_h[row] + g;
            }
            x += col_w[col] + g;
        }
        return c.constrain(Size{.width = total_w, .height = total_h});
    }
};

}  // namespace aurora
