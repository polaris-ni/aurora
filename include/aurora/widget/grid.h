#pragma once

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief `Grid` 的属性权威表：列数与间距各一行，子节点不入表。
///
/// 两行都是**常量档**（回落点是库内默认提供者，不随主题变），故读件一律无参；两行的唯一消费点都在
/// `Grid::on_layout`，档位因此同为布局档。
///
/// 子节点不在本表里：子项只能随控件构造移入（`Grid(std::vector<Node>, GridProps)` 或
/// `Grid{ a, b }`），运行期增删走 `Container::add` / `remove_child`，属性对象不承担容器内容。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class GridProps : public AbstractProps<GridProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：列数；下界 1。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto columns_row() {
        return prop_row<&GridProps::columns_>("columns")
            .layout()
            .bounds("1")
            .fallback(&GridProps::columns_default)
            .note("Column count");
    }

    /// @brief 可空值件：单元格间距（像素）；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto gap_row() {
        return prop_row<&GridProps::gap_>("gap")
            .layout()
            .bounds("0")
            .fallback(&GridProps::gap_default)
            .note("Cell spacing (px)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() { return make_prop_rows(columns_row(), gap_row()); }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前生效的列数。
    /// @return 生效列数；未表态时为 1。
    [[nodiscard]] auto columns() const -> int { return stated(columns_row()); }

    /// @brief 当前生效的单元格间距（像素）。
    /// @return 生效间距；未表态时为 4.0F。
    [[nodiscard]] auto gap() const -> float { return stated(gap_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入列数。
    /// @param value 新列数；写入件原样存入（下界由反序列化通道把关，布局入口另按「小于 1 视为 1」保底）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_columns(int value) -> GridProps & { return assign(columns_row(), value); }

    /// @brief 装入单元格间距（像素）。
    /// @param value 新间距；0 是「显式不留间距」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_gap(float value) -> GridProps & { return assign(gap_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 列数回落基线。
    /// @return 1。
    [[nodiscard]] static auto columns_default() -> int { return 1; }

    /// @brief 间距回落基线。
    /// @return 4.0F。
    [[nodiscard]] static auto gap_default() -> float { return 4.0F; }

  private:
    std::optional<int> columns_;  ///< 列数表态位
    std::optional<float> gap_;  ///< 单元格间距表态位（像素）

    friend class AbstractProps<GridProps>;
};

/// @brief 网格布局容器：按行优先把子项排进固定列数的网格。
///
/// 每列宽 = 该列最宽子项自然宽；每行高 = 该行最高子项自然高。
/// 若父约束给定有限宽度，则每列宽均分该宽度（子项在列宽约束下测量）。
///
/// 属性面全部经 `GridProps` 的读写件访问；子节点不入属性表：它们只能随构造移入
/// （`Grid(std::vector<Node>, GridProps)` 或 `Grid{ a, b }`），运行期增删走
/// `Container::add` / `remove_child`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Grid : public Container, public GridProps {
  public:
    /// @brief 默认构造：无子项、两行未表态（生效值即回落点），并接线宿主。
    Grid() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 网格属性对象，移动进本实例的 `GridProps` 基类子对象。
    explicit Grid(GridProps props) : GridProps(std::move(props)) { bind(*this); }

    /// @brief 子项 + 配置块构造：子项整体移入容器，属性接管后接线宿主。
    /// @param kids 初始子节点（整体移入 `children_`，节点身份不变，外部 `shared_ptr` 别名继续有效）。
    /// @param props 网格属性对象；默认全未表态，生效值即各行回落点。
    Grid(std::vector<Node> kids, GridProps props = {}) : GridProps(std::move(props)) {
        adopt_children(std::move(kids));
        bind(*this);
    }

    /// @brief 便捷构造：扁平罗列子项（Grid{ a, b }），免写 Node{} 与 Props 包裹。
    /// @param kids 初始子项列表（经 set_children 存入容器）。
    Grid(std::initializer_list<Node> kids) {
        set_children(kids);
        bind(*this);
    }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `GridProps`：两行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> GridProps { return {}; }

    /// @brief 设置列数（链式）；转调 `GridProps::set_columns`，返回控件引用。
    /// @param c 列数；写入件原样存入，布局入口按「小于 1 视为 1」保底。
    /// @return Grid 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_columns(int c) -> Grid & {
        GridProps::set_columns(c);
        return *this;
    }

    /// @brief 设置单元格间距（链式）；转调 `GridProps::set_gap`，返回控件引用。
    /// @param g 间距（像素）；负值原样存入（反序列化通道按描述不变量 `gap >= 0` 拒写）。
    /// @return Grid 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_gap(float g) -> Grid & {
        GridProps::set_gap(g);
        return *this;
    }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集响应式来源；Grid 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] StateSink &out) -> void override {}
    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Grid"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Grid"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Grid" 的描述表：`properties` 按 `rows()` 点名顺序，无事件、可容纳多个子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Grid");
        descriptor.children_policy = "multiple";
        descriptor.invariants = {"columns >= 1", "gap >= 0"};
        descriptor.examples = {R"(au::Grid{ au::Text("A"), au::Text("B") })",
                               R"(au::Grid({ au::Text("A"), au::Text("B") }, au::GridProps().set_columns(2)))"};
        return descriptor;
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return 与 `describe_static()` 相同的 WidgetDescriptor（名称/属性/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化自有属性到 `props`：各行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param props 写入目标 JSON 对象（就地填充键值）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }
    /// @brief 从属性 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 读取来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const int cols = std::max(columns(), 1);
        const float g = gap();
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