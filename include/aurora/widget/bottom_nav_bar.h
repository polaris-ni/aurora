#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"  // 带上下文的读件需完整类型以解主题作用域
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.surface / roles.primary 等
#include "aurora/widget/props.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 底部导航栏单项（图标绘制器 + 文案）。
struct BottomNavItem {
    /// @brief 图标绘制器：在给定矩形内绘制（selected 控制配色）。
    /// @param Painter 绘制上下文。
    /// @param Rect 分配给图标的矩形。
    /// @param selected 该项是否选中（决定配色）。
    /// @return 绘制回调无返回值（目标签名返回 void）。
    std::function<void(Painter &, const Rect &, bool selected)> icon;
    /// @brief 项文案（图标下方居中的 12pt 文本）。
    std::string label;
};

/// @brief 底部导航栏属性（权威表）：一行一字段，键名、私有字段、脏档位与回落点只在行里声明一次。
///
/// 读件、写件、序列化与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 七行里两枚值件落常量档（选中下标、栏高），五枚色值落活令牌档（随作用域换肤而变）。
///
/// 两枚**不在表里**的公开事实：导航项列表 `items` 每项含绘制回调、选中回调 `on_select` 本身也是回调，
/// 二者都不是可序列化状态（同 `Button::on_click` 的口径），故留在控件本体的公开槽位，不进属性表。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class BottomNavBarProps : public AbstractProps<BottomNavBarProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：当前选中项下标（高亮 pill 所在 tab）。只改高亮位置 ⇒ 绘制档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto selected_index_row() {
        return prop_row<&BottomNavBarProps::selected_index_>("selected_index")
            .paint()
            .fallback(&BottomNavBarProps::selected_index_default)
            .note("Currently selected item index");
    }

    /// @brief 可空值件：栏高度(px)。既改自身几何又改背景填充区 ⇒ 布局档 + 绘制档。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto bar_height_row() {
        return prop_row<&BottomNavBarProps::bar_height_>("bar_height")
            .layout()
            .paint()
            .fallback(&BottomNavBarProps::bar_height_default)
            .note("Bar height (px)");
    }

    /// @brief 可空值件：栏背景色。未表态时取主题 `roles.surface`，换肤即随动。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto background_color_row() {
        return prop_row<&BottomNavBarProps::background_color_>("background_color")
            .paint()
            .fallback<&ColorRoles::surface>()
            .note("Bar background (follows theme surface when unset)");
    }

    /// @brief 可空值件：顶部分隔线色。未表态时取主题 `roles.outline_variant`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto divider_color_row() {
        return prop_row<&BottomNavBarProps::divider_color_>("divider_color")
            .paint()
            .fallback<&ColorRoles::outline_variant>()
            .note("Top divider line (follows theme outline_variant when unset)");
    }

    /// @brief 可空值件：选中项强调/文字色。未表态时取主题 `roles.primary`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto selected_color_row() {
        return prop_row<&BottomNavBarProps::selected_color_>("selected_color")
            .paint()
            .fallback<&ColorRoles::primary>()
            .note("Selected item text/accent (follows theme primary when unset)");
    }

    /// @brief 可空值件：未选中项文字色。未表态时取主题 `roles.on_surface_variant`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto unselected_color_row() {
        return prop_row<&BottomNavBarProps::unselected_color_>("unselected_color")
            .paint()
            .fallback<&ColorRoles::on_surface_variant>()
            .note("Unselected item text (follows theme on_surface_variant when unset)");
    }

    /// @brief 可空值件：选中 pill 背景色。未表态时取主题 `roles.secondary_container`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto pill_color_row() {
        return prop_row<&BottomNavBarProps::pill_color_>("pill_color")
            .paint()
            .fallback<&ColorRoles::secondary_container>()
            .note("Selected pill background (follows theme secondary_container when unset)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(selected_index_row(), bar_height_row(), background_color_row(), divider_color_row(),
                              selected_color_row(), unselected_color_row(), pill_color_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前选中项下标：常量档回落点不随作用域变 ⇒ 无参读件。
    /// @return 生效选中下标。
    [[nodiscard]] auto selected_index() const -> int { return stated(selected_index_row()); }

    /// @brief 当前生效栏高（px）。
    /// @return 生效栏高。
    [[nodiscard]] auto bar_height() const -> float { return stated(bar_height_row()); }

    /// @brief 当前作用域下生效的栏背景色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.surface`。
    /// @return 生效背景色。
    [[nodiscard]] auto background_color(const BuildContext &ctx) const -> Color {
        return resolve(background_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的顶部分隔线色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.outline_variant`。
    /// @return 生效分隔线色。
    [[nodiscard]] auto divider_color(const BuildContext &ctx) const -> Color {
        return resolve(divider_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的选中项强调/文字色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.primary`。
    /// @return 生效选中项色。
    [[nodiscard]] auto selected_color(const BuildContext &ctx) const -> Color {
        return resolve(selected_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的未选中项文字色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.on_surface_variant`。
    /// @return 生效未选中项色。
    [[nodiscard]] auto unselected_color(const BuildContext &ctx) const -> Color {
        return resolve(unselected_color_row(), ctx);
    }

    /// @brief 当前作用域下生效的选中 pill 背景色。
    /// @param ctx 构建上下文：未表态时据此解主题 `roles.secondary_container`。
    /// @return 生效 pill 背景色。
    [[nodiscard]] auto pill_color(const BuildContext &ctx) const -> Color { return resolve(pill_color_row(), ctx); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入选中下标。
    /// @param value 新选中下标（按等分宽度定位高亮 pill）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_selected_index(int value) -> BottomNavBarProps & { return assign(selected_index_row(), value); }

    /// @brief 装入栏高度（px）。
    /// @param value 新高度，布局时与可用高度取小。
    /// @return 引用本属性对象，便于链式续写。
    auto set_bar_height(float value) -> BottomNavBarProps & { return assign(bar_height_row(), value); }

    /// @brief 装入栏背景色；表态即脱离主题 `roles.surface`。
    /// @param value 新背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_background_color(Color value) -> BottomNavBarProps & { return assign(background_color_row(), value); }

    /// @brief 装入顶部分隔线色；表态即脱离主题 `roles.outline_variant`。
    /// @param value 新分隔线色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_divider_color(Color value) -> BottomNavBarProps & { return assign(divider_color_row(), value); }

    /// @brief 装入选中项强调/文字色；表态即脱离主题 `roles.primary`。
    /// @param value 新强调色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_selected_color(Color value) -> BottomNavBarProps & { return assign(selected_color_row(), value); }

    /// @brief 装入未选中项文字色；表态即脱离主题 `roles.on_surface_variant`。
    /// @param value 新文字色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_unselected_color(Color value) -> BottomNavBarProps & { return assign(unselected_color_row(), value); }

    /// @brief 装入选中 pill 背景色；表态即脱离主题 `roles.secondary_container`。
    /// @param value 新 pill 背景色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_pill_color(Color value) -> BottomNavBarProps & { return assign(pill_color_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 选中下标回落基线。
    /// @return 0（首项）。
    [[nodiscard]] static auto selected_index_default() -> int { return 0; }

    /// @brief 栏高回落基线。
    /// @return 64.0F（px）。
    [[nodiscard]] static auto bar_height_default() -> float { return 64.0F; }

  private:
    std::optional<int> selected_index_;  ///< 选中下标表态位
    std::optional<float> bar_height_;  ///< 栏高表态位（px）
    std::optional<Color> background_color_;  ///< 栏背景色表态位
    std::optional<Color> divider_color_;  ///< 顶部分隔线色表态位
    std::optional<Color> selected_color_;  ///< 选中项强调色表态位
    std::optional<Color> unselected_color_;  ///< 未选中项文字色表态位
    std::optional<Color> pill_color_;  ///< 选中 pill 背景色表态位

    friend class AbstractProps<BottomNavBarProps>;
};

/// @brief 底部导航栏（Material 风格）：等分宽度的若干 tab，选中态高亮。
///
/// 自身为单控件（非容器），点击命中对应 tab 触发 `on_select(index)`。图标由
/// `BottomNavItem::icon` 绘制器以 `Painter` 回调绘制，文案以 `Text` 节点绘制。
/// 选中态由 `selected_index` 行驱动重绘，宿主通常以 `State<int>` 持有并重建页面。
///
/// 属性一律经 `BottomNavBarProps` 的读写件访问（本类公有继承它，故 `bar.set_selected_index(1)` 与
/// `bar.selected_index()` 都直接可用）；导航项与选中回调是控件本体的槽位（回调不可序列化）。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class BottomNavBar : public Widget, public BottomNavBarProps {
  public:
    /// @brief 图标绘制器别名：BottomNavItem::icon 的函数类型。
    /// @param Painter 绘制上下文。
    /// @param Rect 分配给图标的矩形。
    /// @param selected 该项是否选中（决定配色）。
    /// @return 绘制函数无返回值（返回 void）。
    using IconPainter = std::function<void(Painter &, const Rect &, bool selected)>;

    /// @brief 默认构造：七行皆未表态（生效值即各行回落点）；接线宿主。
    BottomNavBar() { bind(*this); }

    /// @brief 以属性对象构造（属性整体接管后接线宿主）。
    /// @param props 属性快照；移动进本实例的 `BottomNavBarProps` 基类子对象。
    explicit BottomNavBar(BottomNavBarProps props) : BottomNavBarProps(std::move(props)) { bind(*this); }

    /// @brief 便捷构造：指定导航项列表，其余属性取回落点或随传入属性对象。
    /// @param its 导航项列表（移动接管）。
    /// @param props 其余属性快照（默认全未表态）。
    explicit BottomNavBar(std::vector<BottomNavItem> its, BottomNavBarProps props = {})
        : BottomNavBar(std::move(props)) {
        set_items(std::move(its));
    }

    /// @brief 导航项列表槽位：栏宽按项数等分、依序排列。
    ///        非属性行——每项含绘制回调，不可序列化，故留在控件本体。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::vector<BottomNavItem> items;

    /// @brief 选中变更回调：点击某 tab 时以新下标同步调用。
    ///        非属性行——它是回调而非可序列化状态，故留在控件本体。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int)> on_select;

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `BottomNavBarProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> BottomNavBarProps { return {}; }

    /// @brief 替换导航项并请求重绘（链式）。
    /// @param its 新导航项列表（移动接管）。
    /// @return *this（链式）。
    auto set_items(std::vector<BottomNavItem> its) -> BottomNavBar & {
        items = std::move(its);
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中回调（链式）；点击 tab 时以新下标调用。
    /// @param cb 回调（移动接管；可空则点击仅标记 handled）。
    /// @return *this（链式）。
    auto set_on_select(std::function<void(int)> cb) -> BottomNavBar & {
        on_select = std::move(cb);
        return *this;
    }

    /// @brief 登记需订阅的信号视图：表里没有来源型行，故只转发采集。
    /// @param out 信号采集出口。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 类型名 "BottomNavBar"。
    /// @return 类型名常量串。
    [[nodiscard]] auto type_name() const -> const char * override { return "BottomNavBar"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "BottomNavBar" 的描述表：`properties` 按 `rows()` 点名顺序，事件含 `on_select`，无子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("BottomNavBar");
        descriptor.events = {{"on_select", "void(int)", "Callback when an item is clicked (argument is the index)"}};
        descriptor.children_policy = "none";
        descriptor.examples = {
            "au::BottomNavBar({au::BottomNavItem{.label = \"Home\"}}, au::BottomNavBarProps().set_selected_index(0))",
            "au::BottomNavBar(au::BottomNavBarProps().set_selected_index(1))"};
        return descriptor;
    }
    /// @brief 实例描述符：转发 describe_static()。
    /// @return 本控件的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化自有属性到 `props`：已表态的行才写键（未表态不写键，回落语义不外泄）。
    /// @param props 目标 JSON 对象（基类通用属性先写入）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief Press 命中按等分格宽换算下标：有效则触发 on_select 并置 is_handled。
    /// @param e 鼠标事件（读 local_position，写 is_handled）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && !items.empty()) {
            const int n = static_cast<int>(items.size());
            const int idx = static_cast<int>(std::floor(e.local_position.x / (size().width / static_cast<float>(n))));
            if (idx >= 0 && idx < n) {
                if (on_select) {
                    on_select(idx);
                }
                e.is_handled = true;
            }
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float h = std::min(bar_height(), c.max.height);
        return c.constrain(Size{.width = c.max.width, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        // 背景条（跟随主题 surface；显式设色优先）
        p.fill_rect(bounds, background_color(ctx));
        // 顶部发丝分隔线（跟随主题 outline_variant）
        p.draw_line(Point{.x = bounds.origin.x, .y = bounds.origin.y},
                    Point{.x = bounds.origin.x + bounds.size.width, .y = bounds.origin.y}, 1.0F, divider_color(ctx));

        const int n = static_cast<int>(items.size());
        if (n == 0) {
            p.pop_clip();
            return;
        }
        const float cell_w = bounds.size.width / static_cast<float>(n);

        for (int i = 0; i < n; ++i) {
            const bool sel = i == selected_index();
            const Rect cell{
                .origin = Point{.x = bounds.origin.x + (static_cast<float>(i) * cell_w), .y = bounds.origin.y},
                .size = Size{.width = cell_w, .height = bounds.size.height}};
            if (sel) {
                const Rect pill{.origin = Point{.x = cell.origin.x + (cell_w * 0.15F), .y = cell.origin.y + 8.0F},
                                .size = Size{.width = cell_w * 0.70F, .height = cell.size.height - 16.0F}};
                // 选中 pill 背景（跟随主题 secondary_container）
                p.fill_rounded_rect(pill, 16.0F, pill_color(ctx));
            }
            // 图标区（顶部居中）
            const Rect icon_rect{
                .origin = Point{.x = cell.origin.x + (cell_w * 0.5F) - 13.0F, .y = cell.origin.y + 10.0F},
                .size = Size{.width = 26.0F, .height = 26.0F}};
            if (items[i].icon) {
                items[i].icon(p, icon_rect, sel);
            }

            // 文案（图标下方居中）：选中→primary，未选中→on_surface_variant（均跟随主题）。
            const Rect label_rect{
                .origin = Point{.x = cell.origin.x + 4.0F, .y = cell.origin.y + cell.size.height - 22.0F},
                .size = Size{.width = cell_w - 8.0F, .height = 16.0F}};
            auto label = std::make_shared<Text>(items[i].label);
            label->text_color = sel ? selected_color(ctx) : unselected_color(ctx);
            label->font.size_pt = 12.0F;
            label->text_align = TextAlign::Center;
            Node ln{label};
            ln.widget().layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = label_rect.size}, ctx);
            ln.widget().paint(p, label_rect, ctx);
        }
        p.pop_clip();
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

// clang 无 "-Wdangling-pointer" 告警组（实测报 -Wunknown-warning-option），故压制只在 GCC 下展开。
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)) {
            return {};
        }
        (void)ctx;
        return std::vector{HitNode{this, weak_from_this(), bounds.origin}};
    }
#ifdef AURORA_COMPILER_GCC
#pragma GCC diagnostic pop
#endif
};

}  // namespace aurora
