#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 底部导航栏单项（图标绘制器 + 文案）。
struct BottomNavItem {
    /// @brief 图标绘制器：在给定矩形内绘制（selected 控制配色）。
    std::function<void(Painter &, const Rect &, bool selected)> icon;
    std::string label;
};

/// @brief 底部导航栏属性（聚合）。
struct BottomNavBarProps {
    std::vector<BottomNavItem> items;
    int selected_index = 0;
    std::function<void(int)> on_select;
    float bar_height = 64.0f;
    float selected_color = 0.0f; ///< 占位：实际配色在 on_paint 内由 selected_index 决定
};

/**
 * @brief 底部导航栏（Material 风格）：等分宽度的若干 tab，选中态高亮。
 *
 * 自身为单控件（非容器），点击命中对应 tab 触发 `on_select(index)`。图标由
 * `BottomNavItem::icon` 绘制器以 `Painter` 回调绘制，文案以 `Text` 节点绘制。
 * 选中态由 `selected_index` 驱动重绘，宿主通常以 `State<int>` 持有并重建页面。
 *
 * 采用继承式双模 API：`BottomNavBarProps` 字段即本控件公有字段。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class BottomNavBar : public Widget, public BottomNavBarProps {
  public:
    using IconPainter = std::function<void(Painter &, const Rect &, bool selected)>;

    BottomNavBar() = default;
    explicit BottomNavBar(BottomNavBarProps props) {
        items = std::move(props.items);
        selected_index = props.selected_index;
        on_select = std::move(props.on_select);
        bar_height = props.bar_height;
    }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "BottomNavBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "BottomNavBar",
            .properties = {
                { .name="selected_index", .type="int", .default_value="0", .required=false, .note="当前选中项下标" },
                { .name="bar_height", .type="float", .default_value="64.0", .required=false, .note="导航栏高度(px)" },
            },
            .events = { { "on_select", "void(int)", "点击某项时回调（参数为下标）" } },
            .children_policy = "none",
            .examples = { "au::BottomNavBar{ au::BottomNavBarProps{ .items = {...}, .on_select = [](int){} } }" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["selected_index"] = selected_index;
        props["bar_height"] = bar_height;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected_index")) {
            selected_index = props["selected_index"].get<int>();
        }
        if (props.contains("bar_height")) {
            bar_height = props["bar_height"].get<float>();
        }
    }

    auto set_items(std::vector<BottomNavItem> its) -> BottomNavBar & {
        items = std::move(its);
        mark_needs_paint();
        return *this;
    }
    auto set_selected_index(int i) -> BottomNavBar & {
        selected_index = i;
        mark_needs_paint();
        return *this;
    }
    auto set_on_select(std::function<void(int)> cb) -> BottomNavBar & {
        on_select = std::move(cb);
        return *this;
    }
    auto set_bar_height(float h) -> BottomNavBar & {
        bar_height = h;
        mark_needs_layout();
        return *this;
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && !items.empty()) {
            const int n = static_cast<int>(items.size());
            const int idx = static_cast<int>(std::floor(e.local_position.x / (size().width / static_cast<float>(n))));
            if (idx >= 0 && idx < n) {
                if (on_select) {
                    on_select(idx);
                }
                e.handled = true;
            }
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float h = std::min(bar_height, c.max.height);
        return c.constrain(Size{ .width = c.max.width, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        p.push_clip(bounds);
        // 背景条
        p.fill_rect(bounds, Color{ 0xFF, 0xFF, 0xFF, 0xFF });
        // 顶部发丝分隔线
        p.draw_line(Point{ .x = bounds.origin.x, .y = bounds.origin.y },
                    Point{ .x = bounds.origin.x + bounds.size.width, .y = bounds.origin.y }, 1.0f,
                    Color{ 0xDA, 0xDC, 0xE0, 0xFF });

        const int n = static_cast<int>(items.size());
        if (n == 0) {
            p.pop_clip();
            return;
        }
        const float cell_w = bounds.size.width / static_cast<float>(n);
        constexpr Color color_blue{ 0x1A, 0x73, 0xE8, 0xFF };
        constexpr Color color_gray{ 0x5F, 0x63, 0x68, 0xFF };
        constexpr Color color_pill{ 0xE8, 0xF0, 0xFE, 0xFF };

        for (int i = 0; i < n; ++i) {
            const bool sel = i == selected_index;
            const Rect cell{ .origin =
                                 Point{ .x = bounds.origin.x + static_cast<float>(i) * cell_w, .y = bounds.origin.y },
                             .size = Size{ .width = cell_w, .height = bounds.size.height } };
            if (sel) {
                const Rect pill{ .origin = Point{ .x = cell.origin.x + cell_w * 0.15f, .y = cell.origin.y + 8.0f },
                                 .size = Size{ .width = cell_w * 0.70f, .height = cell.size.height - 16.0f } };
                p.fill_rounded_rect(pill, 16.0f, color_pill);
            }
            // 图标区（顶部居中）
            const Rect icon_rect{ .origin =
                                      Point{ .x = cell.origin.x + cell_w * 0.5f - 13.0f, .y = cell.origin.y + 10.0f },
                                  .size = Size{ .width = 26.0f, .height = 26.0f } };
            if (items[i].icon) {
                items[i].icon(p, icon_rect, sel);
            }

            // 文案（图标下方居中）
            const Rect label_rect{ .origin = Point{ .x = cell.origin.x + 4.0f,
                                                    .y = cell.origin.y + cell.size.height - 22.0f },
                                   .size = Size{ .width = cell_w - 8.0f, .height = 16.0f } };
            auto label = std::make_shared<Text>(items[i].label);
            label->text_color = sel ? color_blue : color_gray;
            label->font.size_pt = 12.0f;
            label->text_align = TextAlign::Center;
            Node ln{ label };
            ln.widget().layout(Constraints{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = label_rect.size },
                               ctx);
            ln.widget().paint(p, label_rect, ctx);
        }
        p.pop_clip();
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local)) {
            return {};
        }
        (void)ctx;
        return std::vector{ HitNode{ this, weak_from_this(), bounds.origin } };
    }
#pragma GCC diagnostic pop
};

} // namespace aurora
