#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/platform.h"  // NOLINT
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
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

    /// @brief 信号收集：本控件无内建 SignalView，保持输出为空。
    /// @param out 信号视图累加表。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}
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
    }

    /// @brief 反序列化专有属性：selected_index/bar_height 存在才覆盖。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected_index")) {
            selected_index = props.at("selected_index")->as_or<std::int32_t>(0);
        }
        if (props.contains("bar_height")) {
            bar_height = props.at("bar_height")->as_or<float>(0.0F);
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
        // 背景条
        p.fill_rect(bounds, Color{0xFF, 0xFF, 0xFF, 0xFF});
        // 顶部发丝分隔线
        p.draw_line(Point{.x = bounds.origin.x, .y = bounds.origin.y},
                    Point{.x = bounds.origin.x + bounds.size.width, .y = bounds.origin.y}, 1.0F,
                    Color{0xDA, 0xDC, 0xE0, 0xFF});

        const int n = static_cast<int>(items.size());
        if (n == 0) {
            p.pop_clip();
            return;
        }
        const float cell_w = bounds.size.width / static_cast<float>(n);
        constexpr Color color_blue{0x1A, 0x73, 0xE8, 0xFF};
        constexpr Color color_gray{0x5F, 0x63, 0x68, 0xFF};
        constexpr Color color_pill{0xE8, 0xF0, 0xFE, 0xFF};

        for (int i = 0; i < n; ++i) {
            const bool sel = i == selected_index;
            const Rect cell{
                .origin = Point{.x = bounds.origin.x + (static_cast<float>(i) * cell_w), .y = bounds.origin.y},
                .size = Size{.width = cell_w, .height = bounds.size.height}};
            if (sel) {
                const Rect pill{.origin = Point{.x = cell.origin.x + (cell_w * 0.15F), .y = cell.origin.y + 8.0F},
                                .size = Size{.width = cell_w * 0.70F, .height = cell.size.height - 16.0F}};
                p.fill_rounded_rect(pill, 16.0F, color_pill);
            }
            // 图标区（顶部居中）
            const Rect icon_rect{
                .origin = Point{.x = cell.origin.x + (cell_w * 0.5F) - 13.0F, .y = cell.origin.y + 10.0F},
                .size = Size{.width = 26.0F, .height = 26.0F}};
            if (items[i].icon) {
                items[i].icon(p, icon_rect, sel);
            }

            // 文案（图标下方居中）
            const Rect label_rect{
                .origin = Point{.x = cell.origin.x + 4.0F, .y = cell.origin.y + cell.size.height - 22.0F},
                .size = Size{.width = cell_w - 8.0F, .height = 16.0F}};
            auto label = std::make_shared<Text>(items[i].label);
            label->text_color = sel ? color_blue : color_gray;
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
