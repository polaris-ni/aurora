#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"  // inherit_theme：未显式设色时取主题角色
#include "aurora/widget/props_io.h"  // color_to_json / json_to_color
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

/// @brief 底部导航栏属性（聚合）。
struct BottomNavBarProps {
    /// @brief 导航项列表（栏宽按项数等分，依序排列）。
    std::vector<BottomNavItem> items;
    /// @brief 当前选中项下标（高亮 pill 所在 tab）。
    int selected_index = 0;
    /// @brief 选中变更回调：点击某 tab 时以新下标调用。
    /// @return 回调目标签名返回 void（无返回值）。
    std::function<void(int)> on_select;
    /// @brief 栏高度(px)，默认 64；布局时与可用高度取小。
    float bar_height = 64.0F;
    /// @brief 栏背景色；空 = 跟随主题 `surface`（ThemeScope 换肤即生效）。
    std::optional<Color> background_color;
    /// @brief 顶部分隔线色；空 = 跟随主题 `outline_variant`。
    std::optional<Color> divider_color;
    /// @brief 选中项文字/强调色；空 = 跟随主题 `primary`。
    std::optional<Color> selected_color;
    /// @brief 未选中项文字色；空 = 跟随主题 `on_surface_variant`。
    std::optional<Color> unselected_color;
    /// @brief 选中项 pill 背景色；空 = 跟随主题 `secondary_container`。
    std::optional<Color> pill_color;
};

/// @brief 底部导航栏（Material 风格）：等分宽度的若干 tab，选中态高亮。
///
/// 自身为单控件（非容器），点击命中对应 tab 触发 `on_select(index)`。图标由
/// `BottomNavItem::icon` 绘制器以 `Painter` 回调绘制，文案以 `Text` 节点绘制。
/// 选中态由 `selected_index` 驱动重绘，宿主通常以 `State<int>` 持有并重建页面。
///
/// 采用继承式双模 API：`BottomNavBarProps` 字段即本控件公有字段。
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

    /// @brief 默认构造：空项列表、选中下标 0、默认栏高 64。
    BottomNavBar() = default;
    /// @brief 以属性聚合构造：逐字段接管（items/on_select 移动）。
    /// @param props 属性聚合（构造后即弃）。
    explicit BottomNavBar(BottomNavBarProps props) {
        items = std::move(props.items);
        selected_index = props.selected_index;
        on_select = std::move(props.on_select);
        bar_height = props.bar_height;
    }

    /// @brief 信号收集：本控件无内建响应式来源，保持输出为空。
    /// @param out 信号视图累加表。
    auto collect_signals([[maybe_unused]] StateSink &out) -> void override {}
    /// @brief 类型名 "BottomNavBar"。
    /// @return 类型名常量串。
    [[nodiscard]] auto type_name() const -> const char * override { return "BottomNavBar"; }

    /// @brief 静态描述符：属性（选中下标/栏高）与 on_select 事件的元数据。
    /// @return 本控件类型的 WidgetDescriptor。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "BottomNavBar",
            .properties =
                {
                    {.name = "selected_index",
                     .type = "int",
                     .default_value = "0",
                     .required = false,
                     .note = "Currently selected item index"},
                    {.name = "bar_height",
                     .type = "float",
                     .default_value = "64.0",
                     .required = false,
                     .note = "Bar height (px)"},
                    {.name = "background_color",
                     .type = "Color",
                     .default_value = R"("theme.surface")",
                     .required = false,
                     .note = "Bar background (follows theme surface when unset)",
                     .json_type = "array"},
                    {.name = "divider_color",
                     .type = "Color",
                     .default_value = R"("theme.outline_variant")",
                     .required = false,
                     .note = "Top divider line (follows theme outline_variant when unset)",
                     .json_type = "array"},
                    {.name = "selected_color",
                     .type = "Color",
                     .default_value = R"("theme.primary")",
                     .required = false,
                     .note = "Selected item text/accent (follows theme primary when unset)",
                     .json_type = "array"},
                    {.name = "unselected_color",
                     .type = "Color",
                     .default_value = R"("theme.on_surface_variant")",
                     .required = false,
                     .note = "Unselected item text (follows theme on_surface_variant when unset)",
                     .json_type = "array"},
                    {.name = "pill_color",
                     .type = "Color",
                     .default_value = R"("theme.secondary_container")",
                     .required = false,
                     .note = "Selected pill background (follows theme secondary_container when unset)",
                     .json_type = "array"},
                },
            .events = {{"on_select", "void(int)", "Callback when an item is clicked (argument is the index)"}},
            .children_policy = "none",
            .examples = {"au::BottomNavBar{ au::BottomNavBarProps{ .items = {...}, .on_select = [](int){} } }"},
        };
    }
    /// @brief 实例描述符：转发 describe_static()。
    /// @return 本控件的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化专有属性（选中下标与栏高）。
    /// @param props 目标 JSON 对象（基类通用属性先写入）。
    auto serialize_props(Json &props) const -> void override {
        // 先链入基类通用属性。
        Widget::serialize_props(props);
        props.set("selected_index", selected_index);  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        props.set("bar_height", bar_height);
        // 展示色仅在显式设置时输出：保留「跟随主题角色」语义（迁移批）。
        if (background_color.has_value()) {
            props.set("background_color", color_to_json(*background_color));
        }
        if (divider_color.has_value()) {
            props.set("divider_color", color_to_json(*divider_color));
        }
        if (selected_color.has_value()) {
            props.set("selected_color", color_to_json(*selected_color));
        }
        if (unselected_color.has_value()) {
            props.set("unselected_color", color_to_json(*unselected_color));
        }
        if (pill_color.has_value()) {
            props.set("pill_color", color_to_json(*pill_color));
        }
    }

    /// @brief 反序列化专有属性：selected_index/bar_height 存在才覆盖；展示色读入 optional。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected_index")) {
            selected_index = props.at("selected_index")->as_or<std::int32_t>(0);
        }
        if (props.contains("bar_height")) {
            bar_height = props.at("bar_height")->as_or<float>(0.0F);
        }
        if (props.contains("background_color")) {
            background_color = json_to_color(*props.at("background_color"));
        }
        if (props.contains("divider_color")) {
            divider_color = json_to_color(*props.at("divider_color"));
        }
        if (props.contains("selected_color")) {
            selected_color = json_to_color(*props.at("selected_color"));
        }
        if (props.contains("unselected_color")) {
            unselected_color = json_to_color(*props.at("unselected_color"));
        }
        if (props.contains("pill_color")) {
            pill_color = json_to_color(*props.at("pill_color"));
        }
    }

    /// @brief 替换导航项并请求重绘（链式）。
    /// @param its 新导航项列表（移动接管）。
    /// @return *this（链式）。
    auto set_items(std::vector<BottomNavItem> its) -> BottomNavBar & {
        items = std::move(its);
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置选中下标并请求重绘（链式）。
    /// @param i 新选中下标（按等分宽度定位高亮 pill）。
    /// @return *this（链式）。
    auto set_selected_index(int i) -> BottomNavBar & {
        selected_index = i;
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
    /// @brief 设置栏高度并请求重布局（链式）。
    /// @param h 高度(px)，布局时与可用高度取小。
    /// @return *this（链式）。
    auto set_bar_height(float h) -> BottomNavBar & {
        bar_height = h;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置栏背景色（链式）。
    /// @param c 背景色，写入 `background_color`；显式设置后不再跟随主题。
    /// @return *this（链式）。
    auto set_background_color(Color c) -> BottomNavBar & {
        background_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置顶部分隔线色（链式）。
    /// @param c 分隔线色，写入 `divider_color`；显式设置后不再跟随主题。
    /// @return *this（链式）。
    auto set_divider_color(Color c) -> BottomNavBar & {
        divider_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置选中项强调/文字色（链式）。
    /// @param c 强调色，写入 `selected_color`；显式设置后不再跟随主题。
    /// @return *this（链式）。
    auto set_selected_color(Color c) -> BottomNavBar & {
        selected_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置未选中项文字色（链式）。
    /// @param c 文字色，写入 `unselected_color`；显式设置后不再跟随主题。
    /// @return *this（链式）。
    auto set_unselected_color(Color c) -> BottomNavBar & {
        unselected_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置选中 pill 背景色（链式）。
    /// @param c pill 背景色，写入 `pill_color`；显式设置后不再跟随主题。
    /// @return *this（链式）。
    auto set_pill_color(Color c) -> BottomNavBar & {
        pill_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 解析生效栏背景色：未显式设色时经 ctx 取主题 `surface`（ThemeScope 换肤即生效），显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效背景色。
    [[nodiscard]] auto resolve_background(const BuildContext &ctx) const -> Color {
        return background_color.value_or(inherit_theme(ctx).roles.surface);
    }
    /// @brief 解析生效顶部分隔线色：未显式设色时经 ctx 取主题 `outline_variant`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效分隔线色。
    [[nodiscard]] auto resolve_divider(const BuildContext &ctx) const -> Color {
        return divider_color.value_or(inherit_theme(ctx).roles.outline_variant);
    }
    /// @brief 解析生效选中项强调/文字色：未显式设色时经 ctx 取主题 `primary`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效选中项色。
    [[nodiscard]] auto resolve_selected_color(const BuildContext &ctx) const -> Color {
        return selected_color.value_or(inherit_theme(ctx).roles.primary);
    }
    /// @brief 解析生效未选中项文字色：未显式设色时经 ctx 取主题 `on_surface_variant`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效未选中项色。
    [[nodiscard]] auto resolve_unselected_color(const BuildContext &ctx) const -> Color {
        return unselected_color.value_or(inherit_theme(ctx).roles.on_surface_variant);
    }
    /// @brief 解析生效选中 pill 背景色：未显式设色时经 ctx 取主题 `secondary_container`，显式设色优先。
    /// @param ctx 构建上下文：供主题取色；无注入时 `inherit_theme` 回落 light。
    /// @return 生效 pill 背景色。
    [[nodiscard]] auto resolve_pill_color(const BuildContext &ctx) const -> Color {
        return pill_color.value_or(inherit_theme(ctx).roles.secondary_container);
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
        const float h = std::min(bar_height, c.max.height);
        return c.constrain(Size{.width = c.max.width, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        // 背景条（跟随主题 surface；显式设色优先）
        p.fill_rect(bounds, resolve_background(ctx));
        // 顶部发丝分隔线（跟随主题 outline_variant）
        p.draw_line(Point{.x = bounds.origin.x, .y = bounds.origin.y},
                    Point{.x = bounds.origin.x + bounds.size.width, .y = bounds.origin.y}, 1.0F, resolve_divider(ctx));

        const int n = static_cast<int>(items.size());
        if (n == 0) {
            p.pop_clip();
            return;
        }
        const float cell_w = bounds.size.width / static_cast<float>(n);

        for (int i = 0; i < n; ++i) {
            const bool sel = i == selected_index;
            const Rect cell{
                .origin = Point{.x = bounds.origin.x + (static_cast<float>(i) * cell_w), .y = bounds.origin.y},
                .size = Size{.width = cell_w, .height = bounds.size.height}};
            if (sel) {
                const Rect pill{.origin = Point{.x = cell.origin.x + (cell_w * 0.15F), .y = cell.origin.y + 8.0F},
                                .size = Size{.width = cell_w * 0.70F, .height = cell.size.height - 16.0F}};
                // 选中 pill 背景（跟随主题 secondary_container）
                p.fill_rounded_rect(pill, 16.0F, resolve_pill_color(ctx));
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
            label->text_color = sel ? resolve_selected_color(ctx) : resolve_unselected_color(ctx);
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
