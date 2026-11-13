#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/json.h"
#include "aurora/environment/build_context.h"  // 带上下文的读件需完整类型以解主题作用域
#include "aurora/render/painter.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.outline_variant
#include "aurora/theming/control_metrics.h"  // 活令牌回落点：controls.separator_thickness
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 分隔线方向。
enum class Orientation : std::uint8_t {
    Horizontal,  ///< 横向：线沿父宽度水平铺满，厚度为竖直方向线宽。
    Vertical,  ///< 纵向：线沿父高度竖直铺满，厚度为水平方向线宽。
};

/// @brief Orientation → JSON 字符串（线上格式为小写，与迁移前一致）。
/// @param v 方向枚举值。
/// @return `"vertical"` 或 `"horizontal"`。
[[nodiscard]] inline auto orientation_to_json(Orientation v) -> Json {
    return v == Orientation::Vertical ? "vertical" : "horizontal";
}

/// @brief JSON → Orientation（非字符串或未知值回退横向，与迁移前的宽容解析一致）。
/// @param j 方向字符串。
/// @return 匹配的方向；未知名返回 `Orientation::Horizontal`。
[[nodiscard]] inline auto json_to_orientation(const Json &j) -> Orientation {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "vertical") {
            return Orientation::Vertical;
        }
        if (s == "horizontal") {
            return Orientation::Horizontal;
        }
    }
    return Orientation::Horizontal;
}

/// @brief 分隔线方向属性。
template <>
struct PropCodec<Orientation> : detail::EnumPropCodec<Orientation, &orientation_to_json, &json_to_orientation> {
    /// @brief schema 的 C++ 类型名。
    /// @return `Orientation`。
    [[nodiscard]] static constexpr auto type_text() -> std::string_view { return "Orientation"; }

    /// @brief 合法值表（发射 schema 的 `enum` 字段；拼写沿用枚举成员名）。
    /// @return `{"Horizontal", "Vertical"}`。
    [[nodiscard]] static auto enum_names() -> std::vector<std::string_view> { return {"Horizontal", "Vertical"}; }
};

/// @brief Divider 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 三态语义与迁移前一致，但「未设」不再靠 `std::optional` 外泄表达：
/// - **可空行**（全部五行）：未表态时取该行回落点，表态后不再跟随；撤表态走公共面上唯一的 `clear(row)`；
/// - **活令牌档**（`thickness` / `color`）：回落点随作用域换肤而变，故读件带 `BuildContext`；
/// - **常量档**（`orientation` / `indent` / `end_indent`）：回落点是全库唯一基线，故读件无参。
///
/// 迁移前两行用 `std::optional` 的「空」表达「跟随主题」，三行用非 `optional` 表达「有常量默认」——
/// 现在两者统一为「可空 ⇔ 非必填 + 行上回落点」，差异只剩落到活令牌还是落到常量。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class DividerProps : public AbstractProps<DividerProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：分隔线方向。改变主轴 ⇒ 布局 + 绘制两档都打；回落点是常量横向。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto orientation_row() {
        return prop_row<&DividerProps::orientation_>("orientation")
            .layout()
            .paint()
            .fallback(&DividerProps::orientation_default)
            .note("Direction");
    }

    /// @brief 可空值件：线厚（dp）。未表态时取主题度量档 `controls.separator_thickness`；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto thickness_row() {
        return prop_row<&DividerProps::thickness_>("thickness")
            .layout()
            .paint()
            .bounds("0")
            .fallback<&ControlMetrics::separator_thickness>()
            .note("Line thickness (dp; follows theme controls.separator_thickness when unset)");
    }

    /// @brief 可空值件：线颜色。未表态时取主题 `roles.outline_variant`，换肤即随动。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto color_row() {
        return prop_row<&DividerProps::color_>("color").paint().fallback<&ColorRoles::outline_variant>().note(
            "Line color (follows theme outline_variant when unset)");
    }

    /// @brief 可空值件：起点缩进（dp）；横向为左缩进，纵向为上缩进。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto indent_row() {
        return prop_row<&DividerProps::indent_>("indent")
            .paint()
            .bounds("0")
            .fallback(&DividerProps::indent_default)
            .note("Start indent (dp)");
    }

    /// @brief 可空值件：终点缩进（dp）；横向为右缩进，纵向为下缩进。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto end_indent_row() {
        return prop_row<&DividerProps::end_indent_>("end_indent")
            .paint()
            .bounds("0")
            .fallback(&DividerProps::end_indent_default)
            .note("End indent (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(orientation_row(), thickness_row(), color_row(), indent_row(), end_indent_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前生效的方向：常量档回落点不随作用域变 ⇒ 无参读件。
    /// @return 生效方向。
    [[nodiscard]] auto orientation() const -> Orientation { return stated(orientation_row()); }

    /// @brief 当前作用域下生效的线厚（dp）。
    /// @param ctx 构建上下文：未表态时据此解主题 `controls.separator_thickness`。
    /// @return 生效线厚。
    [[nodiscard]] auto thickness(const BuildContext &ctx) const -> float { return resolve(thickness_row(), ctx); }

    /// @brief 当前作用域下生效的线颜色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.outline_variant`。
    /// @return 生效线颜色。
    [[nodiscard]] auto color(const BuildContext &ctx) const -> Color { return resolve(color_row(), ctx); }

    /// @brief 当前生效的起点缩进（dp）。
    /// @return 生效起点缩进。
    [[nodiscard]] auto indent() const -> float { return stated(indent_row()); }

    /// @brief 当前生效的终点缩进（dp）。
    /// @return 生效终点缩进。
    [[nodiscard]] auto end_indent() const -> float { return stated(end_indent_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入方向；表态即脱离横向基线。
    /// @param value 新方向。
    /// @return 引用本属性对象，便于链式续写。
    auto set_orientation(Orientation value) -> DividerProps & { return assign(orientation_row(), value); }

    /// @brief 装入线厚（dp）；表态即脱离主题度量档。
    /// @param value 新线厚；0 是「显式要一条不画出来的线」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_thickness(float value) -> DividerProps & { return assign(thickness_row(), value); }

    /// @brief 装入线颜色；表态即脱离主题 `roles.outline_variant`。
    /// @param value 新线颜色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(Color value) -> DividerProps & { return assign(color_row(), value); }

    /// @brief 装入起点缩进（dp）。
    /// @param value 新起点缩进；横向为左缩进，纵向为上缩进。
    /// @return 引用本属性对象，便于链式续写。
    auto set_indent(float value) -> DividerProps & { return assign(indent_row(), value); }

    /// @brief 装入终点缩进（dp）。
    /// @param value 新终点缩进；横向为右缩进，纵向为下缩进。
    /// @return 引用本属性对象，便于链式续写。
    auto set_end_indent(float value) -> DividerProps & { return assign(end_indent_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 方向回落基线。
    /// @return `Orientation::Horizontal`（默认横向）。
    [[nodiscard]] static auto orientation_default() -> Orientation { return Orientation::Horizontal; }

    /// @brief 起点缩进回落基线。
    /// @return 0.0F（不缩进）。
    [[nodiscard]] static auto indent_default() -> float { return 0.0F; }

    /// @brief 终点缩进回落基线。
    /// @return 0.0F（不缩进）。
    [[nodiscard]] static auto end_indent_default() -> float { return 0.0F; }

  private:
    std::optional<Orientation> orientation_;  ///< 方向表态位
    std::optional<float> thickness_;  ///< 线厚表态位（dp）
    std::optional<Color> color_;  ///< 线色表态位
    std::optional<float> indent_;  ///< 起点缩进表态位（dp）
    std::optional<float> end_indent_;  ///< 终点缩进表态位（dp）

    friend class AbstractProps<DividerProps>;
};

/// @brief 分隔线（叶控件）：绘制一条视觉分隔线（headless 占位渲染）。
///
/// 横向填满父宽度、纵向填满父高度，厚度与颜色都按两态取值：`set_thickness` / `set_color` 表态即用
/// 显式值，未表态分别跟随主题 `controls.separator_thickness` 与 `roles.outline_variant`。
///
/// 属性一律经 `DividerProps` 的读写件访问（本类公有继承它，故 `div.set_indent(8.0F)` 与
/// `div.indent()` 都直接可用）；`div.thickness(ctx)` / `div.color(ctx)` 取当前作用域下的生效值。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Divider : public LeafWidget, public DividerProps {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }

    /// @brief 默认构造：横向、未设线厚（跟随主题 `controls.separator_thickness`）、未设色（跟随主题
    ///        `roles.outline_variant`）、无缩进；并接线宿主。
    Divider() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 分隔线属性对象，移动进本实例的 `DividerProps` 基类子对象。
    explicit Divider(DividerProps props) : DividerProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `DividerProps`：各行未表态，生效值即该行回落点
    [[nodiscard]] static auto defaults() -> DividerProps { return {}; }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Divider"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Divider"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Divider" 的描述表：`properties` 按 `rows()` 点名顺序，无事件、无子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Divider");
        descriptor.children_policy = "none";
        descriptor.examples = {"au::Divider()",
                               "au::Divider(au::DividerProps().set_orientation(Orientation::Vertical))"};
        return descriptor;
    }

    /// @brief 运行时自描述：转发静态描述符。
    /// @return Divider 的控件描述符（方向/线粗/颜色/缩进与控件级尺寸属性，无事件、无子项）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：本表五行皆为值形态，没有响应式来源可登记。
    /// @param out 输出参数；分隔线无信号，恒不写入。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 序列化自有属性到 `props`：各行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param props 输出目标 JSON 对象；先由基类写 width/height/show，再叠本表行。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 序列化属性对象，按权威表逐行取键。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float t = thickness(ctx);  // 未表态 → 跟随主题度量档
        if (orientation() == Orientation::Vertical) {
            return c.constrain(Size{.width = t, .height = c.max.height});
        }
        return c.constrain(Size{.width = c.max.width, .height = t});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Color line = color(ctx);  // 未表态 → 跟随主题 outline_variant
        const float t = thickness(ctx);  // 未表态 → 跟随主题 separator_thickness
        if (orientation() == Orientation::Vertical) {
            const float y0 = bounds.origin.y + indent();
            const float h = bounds.size.height - indent() - end_indent();
            if (h > 0.0F) {
                const Rect r{.origin = Point{.x = bounds.origin.x, .y = y0}, .size = Size{.width = t, .height = h}};
                p.fill_rect(r, line);
            }
        } else {
            const float x0 = bounds.origin.x + indent();
            const float w = bounds.size.width - indent() - end_indent();
            if (w > 0.0F) {
                const Rect r{.origin = Point{.x = x0, .y = bounds.origin.y}, .size = Size{.width = w, .height = t}};
                p.fill_rect(r, line);
            }
        }
    }
};

}  // namespace aurora
