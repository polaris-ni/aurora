#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 顶级菜单：菜单名 + 菜单项列表（复用 app/menu.h 的 MenuItem）。
struct Menu {
    std::string title;           ///< 顶级菜单名（如 "File"）
    std::vector<MenuItem> items; ///< 下拉菜单项
};

/**
 * @brief 声明式菜单栏（规格 §1.1）：自绘跨平台实现。
 *
 * 顶部一行顶级菜单，点击展开下拉；点击菜单项触发回调并收起；
 * 点击其他区域收起。自绘实现对 Headless/GLFW/Win32 全部可用；
 * Win32 原生 HMENU 映射作为后续增强（当前统一自绘保证行为一致）。
 *
 * 对标 Qt `QMenuBar`/`QMenu`、WPF `Menu`/`MenuItem`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class MenuBar : public Widget {
  public:
    MenuBar() = default;
    explicit MenuBar(std::vector<Menu> menus) : m_menus(std::move(menus)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "MenuBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "MenuBar",
            .properties = {
                { .name = "bar_height", .type = "float", .default_value = "28.0", .required = false, .note = "菜单栏高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "open_menu", .type = "int", .default_value = "-1", .required = false, .note = "当前展开的菜单序号(-1=无)", .json_type = "integer" },
            },
            .events = {},
            .children_policy = "none",
            .examples = { R"(au::MenuBar({ {"File", {au::MenuItem{"Open", fn}}} }))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    [[nodiscard]] auto menu_count() const -> std::size_t { return m_menus.size(); }
    [[nodiscard]] auto open_menu() const -> int { return m_open; }
    [[nodiscard]] auto is_open() const -> bool { return m_open >= 0; }

    /// @brief 展开指定顶级菜单（越界忽略；-1 收起）。
    auto open(int index) -> void {
        if (index >= -1 && std::cmp_less(index, m_menus.size())) {
            m_open = index;
            mark_needs_paint();
        }
    }

    /// @brief 收起下拉。
    auto close() -> void { open(-1); }

    /// @brief 追加顶级菜单。
    auto add_menu(Menu menu) -> void {
        m_menus.push_back(std::move(menu));
        mark_needs_layout();
    }

    /// @brief 设置菜单栏高度（链式）。
    auto set_bar_height(float h) -> MenuBar & {
        m_bar_height = h > 0.0f ? h : 28.0f;
        return *this;
    }
    [[nodiscard]] auto bar_height() const -> float { return m_bar_height; }

    /// @brief 点击交互：栏上点击展开/切换；下拉内点击触发菜单项；其他区域收起。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action != MouseAction::Press) {
            Widget::on_pointer_event(e);
            return;
        }
        // 1) 点击菜单栏：展开/收起对应顶级菜单
        if (e.local_position.y < m_bar_height) {
            float x = 0.0f;
            for (std::size_t i = 0; i < m_menus.size(); ++i) {
                const float w = title_width(i);
                if (e.local_position.x >= x && e.local_position.x < x + w) {
                    m_open = std::cmp_equal(m_open, i) ? -1 : static_cast<int>(i);
                    mark_needs_paint();
                    e.handled = true;
                    return;
                }
                x += w;
            }
            // 栏上空白：收起
            close();
            e.handled = true;
            return;
        }
        // 2) 下拉展开中：命中菜单项则触发
        if (is_open()) {
            const Rect drop = dropdown_bounds();
            if (drop.contains(e.local_position)) {
                const int item_idx = static_cast<int>((e.local_position.y - drop.origin.y) / m_aurora_item_height);
                const auto &items = m_menus[static_cast<std::size_t>(m_open)].items;
                if (item_idx >= 0 && std::cmp_less(item_idx, items.size())) {
                    const MenuItem &item = items[static_cast<std::size_t>(item_idx)];
                    if (!item.separator && item.enabled && item.on_click) {
                        item.on_click();
                    }
                }
                close();
                e.handled = true;
                return;
            }
            // 下拉外点击：收起（消费本次点击）
            close();
            e.handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 当前展开下拉的全局盒（局部坐标，相对本控件原点）。
    [[nodiscard]] auto dropdown_bounds() const -> Rect {
        if (!is_open()) {
            return Rect{};
        }
        float x = 0.0f;
        for (int i = 0; i < m_open; ++i) {
            x += title_width(static_cast<std::size_t>(i));
        }
        const auto &items = m_menus[static_cast<std::size_t>(m_open)].items;
        const float h = static_cast<float>(items.size()) * m_aurora_item_height;
        return Rect{ .origin = Point{ .x = x, .y = m_bar_height },
                     .size = Size{ .width = m_aurora_dropdown_width, .height = h } };
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["bar_height"] = m_bar_height;
        Json titles = Json::array();
        for (const auto &m : m_menus) {
            titles.push_back(m.title);
        }
        props["menu_titles"] = titles;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        // 菜单栏占满宽度、固定高度（下拉为覆盖绘制，不占布局）
        const float w = c.max.is_finite() ? c.max.width : 640.0f;
        return c.constrain(Size{ .width = w, .height = m_bar_height });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        // 栏背景
        const Rect bar{ .origin = bounds.origin, .size = Size{ .width = bounds.size.width, .height = m_bar_height } };
        p.fill_rect(bar, Color(248, 248, 250, 255));

        Font f;
        f.size_pt = 13.0f;

        // 顶级菜单标题
        float x = bounds.origin.x;
        for (std::size_t i = 0; i < m_menus.size(); ++i) {
            const float w = title_width(i);
            const bool active = std::cmp_equal(i, m_open);
            if (active) {
                p.fill_rect(Rect{ .origin = Point{ .x = x, .y = bounds.origin.y },
                                  .size = Size{ .width = w, .height = m_bar_height } },
                            Color(0, 122, 255, 40));
            }
            const Rect text_box{ .origin = Point{ .x = x + m_aurora_title_padding, .y = bounds.origin.y + 6.0f },
                                 .size = Size{ .width = w - (m_aurora_title_padding * 2.0f),
                                               .height = m_bar_height - 12.0f } };
            p.draw_text(text_box, m_menus[i].title, f, Color(30, 30, 30, 255));
            x += w;
        }

        // 展开的下拉
        if (is_open()) {
            const Rect drop_local = dropdown_bounds();
            const Rect drop{ .origin = Point{ .x = bounds.origin.x + drop_local.origin.x,
                                              .y = bounds.origin.y + drop_local.origin.y },
                             .size = drop_local.size };
            p.draw_shadow(drop, 0.0f, 2.0f, 8.0f, Color(0, 0, 0, 48));
            p.fill_rect(drop, Color(255, 255, 255, 255));
            p.draw_rect(drop, Color(220, 220, 224, 255));

            const auto &items = m_menus[static_cast<std::size_t>(m_open)].items;
            float y = drop.origin.y;
            for (const auto &item : items) {
                if (item.separator) {
                    const float mid = y + (m_aurora_item_height * 0.5f);
                    p.fill_rect(Rect{ .origin = Point{ .x = drop.origin.x + 4.0f, .y = mid },
                                      .size = Size{ .width = drop.size.width - 8.0f, .height = 1.0f } },
                                Color(228, 228, 232, 255));
                } else {
                    const Color text_color = item.enabled ? Color(30, 30, 30, 255) : Color(170, 170, 170, 255);
                    const Rect text_box{ .origin = Point{ .x = drop.origin.x + 10.0f, .y = y + 5.0f },
                                         .size = Size{ .width = drop.size.width - 20.0f,
                                                       .height = m_aurora_item_height - 10.0f } };
                    std::string label = item.label;
                    if (item.checkable && item.checked) {
                        label.insert(0, "v ");
                    }
                    p.draw_text(text_box, label, f, text_color);
                    // 快捷键提示右对齐（简化：绘制在右侧固定区）
                    if (!item.shortcut_text.empty()) {
                        const Rect sc_box{ .origin =
                                               Point{ .x = drop.origin.x + drop.size.width - 70.0f, .y = y + 5.0f },
                                           .size = Size{ .width = 60.0f, .height = m_aurora_item_height - 10.0f } };
                        p.draw_text(sc_box, item.shortcut_text, f, Color(150, 150, 150, 255));
                    }
                }
                y += m_aurora_item_height;
            }
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 栏区域 + 展开的下拉区域命中自身
        if (local.y < m_bar_height &&
            Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local)) {
            return this;
        }
        if (is_open() && dropdown_bounds().contains(local)) {
            return this;
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point & /*local*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> std::vector<HitNode> override {
        return {}; // 自身即最深命中（基类前置 this）
    }

  private:
    static constexpr float m_aurora_title_padding = 10.0f;   ///< 顶级菜单标题左右内边距(dp)
    static constexpr float m_aurora_item_height = 26.0f;     ///< 下拉菜单项行高(dp)
    static constexpr float m_aurora_dropdown_width = 180.0f; ///< 下拉菜单宽度(dp)

    /// @brief 顶级菜单标题宽度。
    [[nodiscard]] auto title_width(std::size_t i) const -> float {
        Font f;
        f.size_pt = 13.0f;
        return std::max(render::FontEngine::measure_width(m_menus[i].title, f) + (m_aurora_title_padding * 2.0f),
                        40.0f);
    }

    std::vector<Menu> m_menus;
    int m_open = -1;
    float m_bar_height = 28.0f;
};

} // namespace aurora
