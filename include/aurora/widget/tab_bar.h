#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h" // inherit_theme：active_color 未显式设置时跟随主题 primary
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 单个选项卡描述：标签文本 + 内容子树 + 可选可关闭标记。
struct Tab {
    std::string label;     ///< 标签文本
    Node content;          ///< 选中时显示的内容
    bool closable = false; ///< 是否显示关闭按钮
};

/**
 * @brief 选项卡容器（规格 §3.1）：标签栏 + 内容区。
 *
 * 顶部渲染标签栏（选中项高亮下划线），下方渲染当前选中标签的内容。
 * 点击标签切换选中；`selected_index()` 为响应式状态可订阅。
 *
 * 可定制性（对标 Qt QTabWidget / Flutter TabBar）：
 * - `active_color`（选中文本 + 指示器）未显式设置时跟随主题 `Theme::primary`；
 * - 标签栏背景 / 选中标签背景 / 未选中文本色 / 指示器厚度 / 字号 / 标签内边距均可配。
 *
 * 继承扩展点（protected 虚函数）：`paint_tab`（单个标签，含选中背景/指示器/文本/关闭钮）。
 *
 * 对标 Qt `QTabWidget`、WPF `TabControl`、Flutter `TabBar`+`TabBarView`、SwiftUI `TabView`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class TabBar : public Widget {
  public:
    TabBar() = default;
    explicit TabBar(std::vector<Tab> tabs, int initial = 0) : m_tabs(std::move(tabs)) {
        const int max_idx = static_cast<int>(m_tabs.size()) - 1;
        m_selected.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "TabBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "TabBar",
            .properties = {
                { .name = "selected_index", .type = "int", .default_value = "0", .required = false, .note = "当前选中标签序号", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "tab_height", .type = "float", .default_value = "36.0", .required = false, .note = "标签栏高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "active_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "选中文本与指示器色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "bar_background", .type = "Color", .default_value = "{245,245,247,255}", .required = false, .note = "标签栏背景色", .json_type = "array" },
                { .name = "tab_background", .type = "Color", .default_value = "Color::white()", .required = false, .note = "选中标签背景色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "{96,96,96,255}", .required = false, .note = "未选中标签文本色", .json_type = "array" },
                { .name = "indicator_thickness", .type = "float", .default_value = "2.0", .required = false, .note = "选中指示器厚度(dp)；0=不画", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "font_size", .type = "float", .default_value = "13.0", .required = false, .note = "标签字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "tab_padding", .type = "float", .default_value = "12.0", .required = false, .note = "标签左右内边距(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            },
            .events = { "on_change", "on_close" },
            .children_policy = "multiple",
            .invariants = { "selected_index >= 0", "tab_height > 0" },
            .examples = { R"(au::TabBar({ {"Tab A", au::Text("A")}, {"Tab B", au::Text("B")} }))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    /// @brief 当前选中序号（响应式状态）。
    [[nodiscard]] auto selected_index() -> State<int> & { return m_selected; }
    [[nodiscard]] auto selected() const -> int { return m_selected.get(); }

    /// @brief 切换选中标签（越界忽略）。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, m_tabs.size()) && index != m_selected.get()) {
            m_selected.set(index);
            mark_needs_layout();
            mark_needs_paint();
            if (m_on_change) {
                m_on_change(index);
            }
        }
    }

    /// @brief 关闭标签（移除并回调；选中项自动修正）。
    auto close_tab(int index) -> void {
        if (index < 0 || std::cmp_greater_equal(index, m_tabs.size())) {
            return;
        }
        m_tabs.erase(m_tabs.begin() + index);
        if (m_on_close) {
            m_on_close(index);
        }
        // 修正选中：若删除的在选中之前或就是选中项，前移
        const int sel = m_selected.get();
        if (index <= sel && sel > 0) {
            m_selected.set(sel - 1);
        } else if (std::cmp_greater_equal(sel, m_tabs.size()) && !m_tabs.empty()) {
            m_selected.set(static_cast<int>(m_tabs.size()) - 1);
        }
        mark_needs_layout();
        mark_needs_paint();
    }

    /// @brief 追加标签。
    auto add_tab(Tab tab) -> void {
        m_tabs.push_back(std::move(tab));
        mark_needs_layout();
    }

    [[nodiscard]] auto tab_count() const -> std::size_t { return m_tabs.size(); }
    [[nodiscard]] auto tab_label(int index) const -> std::string {
        if (index < 0 || std::cmp_greater_equal(index, m_tabs.size())) {
            return {};
        }
        return m_tabs[static_cast<std::size_t>(index)].label;
    }

    /// @brief 设置切换回调（链式）。
    auto set_on_change(std::function<void(int)> cb) -> TabBar & {
        m_on_change = std::move(cb);
        return *this;
    }
    /// @brief 设置关闭回调（链式）。
    auto set_on_close(std::function<void(int)> cb) -> TabBar & {
        m_on_close = std::move(cb);
        return *this;
    }
    /// @brief 设置标签栏高度（链式）。
    auto set_tab_height(float h) -> TabBar & {
        m_tab_height = h > 0.0f ? h : 36.0f;
        return *this;
    }

    /// @brief 设置选中文本与指示器色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> TabBar & {
        m_active_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置标签栏背景色（链式）。
    auto set_bar_background(Color c) -> TabBar & {
        m_bar_background = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中标签背景色（链式）。
    auto set_tab_background(Color c) -> TabBar & {
        m_tab_background = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置未选中标签文本色（链式）。
    auto set_text_color(Color c) -> TabBar & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中指示器厚度 dp（链式；0 = 不画指示器）。
    auto set_indicator_thickness(float t) -> TabBar & {
        m_indicator_thickness = t >= 0.0f ? t : 2.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置标签字号 pt（链式）。
    auto set_font_size(float s) -> TabBar & {
        m_font_size = s > 0.0f ? s : 13.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置标签左右内边距 dp（链式）。
    auto set_tab_padding(float pad) -> TabBar & {
        m_tab_padding = pad >= 0.0f ? pad : 12.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 命中标签栏时的点击切换（含关闭按钮命中）。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.local_position.y < m_tab_height) {
            // 查找命中的标签
            float x = 0.0f;
            for (std::size_t i = 0; i < m_tabs.size(); ++i) {
                const float w = tab_width(i);
                if (e.local_position.x >= x && e.local_position.x < x + w) {
                    // 命中关闭按钮区域（标签右侧 16dp）
                    if (m_tabs[i].closable && e.local_position.x > x + w - m_aurora_close_zone) {
                        close_tab(static_cast<int>(i));
                    } else {
                        select(static_cast<int>(i));
                    }
                    e.handled = true;
                    return;
                }
                x += w;
            }
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["selected_index"] = m_selected.get();
        props["tab_height"] = m_tab_height;
        Json labels = Json::array();
        for (const auto &t : m_tabs) {
            labels.push_back(t.label);
        }
        props["tab_labels"] = labels;
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["bar_background"] = color_to_json(m_bar_background);
        props["tab_background"] = color_to_json(m_tab_background);
        props["text_color"] = color_to_json(m_text_color);
        props["indicator_thickness"] = m_indicator_thickness;
        props["font_size"] = m_font_size;
        props["tab_padding"] = m_tab_padding;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected_index")) {
            m_selected.set(props["selected_index"].get<int>());
        }
        if (props.contains("tab_height")) {
            m_tab_height = props["tab_height"].get<float>();
        }
        if (props.contains("active_color")) {
            m_active_color = json_to_color(props["active_color"]);
        }
        if (props.contains("bar_background")) {
            m_bar_background = json_to_color(props["bar_background"]);
        }
        if (props.contains("tab_background")) {
            m_tab_background = json_to_color(props["tab_background"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("indicator_thickness")) {
            m_indicator_thickness = props["indicator_thickness"].get<float>();
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("tab_padding")) {
            m_tab_padding = props["tab_padding"].get<float>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        for (const auto &t : m_tabs) {
            if (t.content) {
                fn(t.content.widget());
            }
        }
    }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        m_child_view.clear();
        m_child_view.reserve(m_tabs.size());
        for (const auto &t : m_tabs) {
            if (t.content) {
                m_child_view.push_back(t.content);
            }
        }
        return m_child_view;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 480.0f, .height = 320.0f };
        }
        // 当前内容区布局
        const int sel = m_selected.get();
        if (sel >= 0 && std::cmp_less(sel, m_tabs.size()) && m_tabs[static_cast<std::size_t>(sel)].content) {
            Node &content = m_tabs[static_cast<std::size_t>(sel)].content;
            Constraints inner;
            inner.min = Size{ .width = 0.0f, .height = 0.0f };
            inner.max = Size{ .width = self.width, .height = std::max(0.0f, self.height - m_tab_height) };
            const Size cs = content.widget().layout(inner, ctx);
            content.widget().set_layout_parent(this);
            content.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = m_tab_height }, .size = cs });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 标签栏背景
        const Rect bar{ .origin = bounds.origin, .size = Size{ .width = bounds.size.width, .height = m_tab_height } };
        p.fill_rect(bar, m_bar_background);

        // 选中色：显式设置优先，否则跟随主题 primary（ThemeScope 换肤即生效）。
        const Color accent = m_active_color.value_or(inherit_theme(ctx).primary);
        Font label_font;
        label_font.size_pt = m_font_size;

        float x = bounds.origin.x;
        for (std::size_t i = 0; i < m_tabs.size(); ++i) {
            const float w = tab_width(i);
            const Rect tab_box{ .origin = Point{ .x = x, .y = bounds.origin.y },
                                .size = Size{ .width = w, .height = m_tab_height } };
            paint_tab(p, i, tab_box, std::cmp_equal(i, m_selected.get()), accent, label_font);
            x += w;
        }

        // 当前内容
        const int sel = m_selected.get();
        if (sel >= 0 && std::cmp_less(sel, m_tabs.size()) && m_tabs[static_cast<std::size_t>(sel)].content) {
            Node &content = m_tabs[static_cast<std::size_t>(sel)].content;
            const Rect cb = content.bounds();
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            content.widget().paint(p, global, ctx);
        }
    }

    /// @brief 继承扩展点：绘制单个标签（选中背景 + 指示器 + 文本 + 可选关闭钮）。
    virtual auto paint_tab(Painter &p, std::size_t index, const Rect &tab_box, bool selected, Color accent,
                           const Font &label_font) -> void {
        if (selected) {
            p.fill_rect(tab_box, m_tab_background);
            if (m_indicator_thickness > 0.0f) {
                const Rect underline{ .origin = Point{ .x = tab_box.origin.x,
                                                       .y = tab_box.origin.y + m_tab_height - m_indicator_thickness },
                                      .size = Size{ .width = tab_box.size.width, .height = m_indicator_thickness } };
                p.fill_rect(underline, accent);
            }
        }
        const Color text_color = selected ? accent : m_text_color;
        const Rect text_box{ .origin = Point{ .x = tab_box.origin.x + m_tab_padding, .y = tab_box.origin.y + 8.0f },
                             .size = Size{ .width = tab_box.size.width - (m_tab_padding * 2.0f),
                                           .height = m_tab_height - 16.0f } };
        p.draw_text(text_box, m_tabs[index].label, label_font, text_color);
        // 关闭按钮
        if (m_tabs[index].closable) {
            const Rect close_box{ .origin =
                                      Point{ .x = tab_box.origin.x + tab_box.size.width - m_aurora_close_zone + 2.0f,
                                             .y = tab_box.origin.y + 10.0f },
                                  .size = Size{ .width = 12.0f, .height = m_tab_height - 20.0f } };
            p.draw_text(close_box, "x", label_font, Color{ 140, 140, 140, 255 });
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 标签栏区域命中自身
        if (local.y < m_tab_height) {
            return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this
                                                                                                        : nullptr;
        }
        // 内容区命中当前选中子树
        const int sel = m_selected.get();
        if (sel >= 0 && std::cmp_less(sel, m_tabs.size()) && m_tabs[static_cast<std::size_t>(sel)].content) {
            Node &content = m_tabs[static_cast<std::size_t>(sel)].content;
            const Rect cb = content.bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                return content.widget().hit_test(local - cb.origin, global, ctx);
            }
        }
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (local.y >= m_tab_height) {
            const int sel = m_selected.get();
            if (sel >= 0 && std::cmp_less(sel, m_tabs.size()) && m_tabs[static_cast<std::size_t>(sel)].content) {
                Node &content = m_tabs[static_cast<std::size_t>(sel)].content;
                const Rect cb = content.bounds();
                if (cb.contains(local)) {
                    const Rect global{ .origin = Point{ .x = bounds.origin.x + cb.origin.x,
                                                        .y = bounds.origin.y + cb.origin.y },
                                       .size = cb.size };
                    return content.widget().hit_test_chain(local - cb.origin, global, ctx);
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        for (auto &t : m_tabs) {
            if (t.content) {
                t.content.widget().mount(ctx);
            }
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        for (auto &t : m_tabs) {
            if (t.content) {
                t.content.widget().tick(now);
            }
        }
    }

    static constexpr float m_aurora_tab_padding = 12.0f; ///< 标签左右内边距默认值(dp)
    static constexpr float m_aurora_close_zone = 20.0f;  ///< 关闭按钮命中区宽度(dp)

    /// @brief 单个标签宽度：文本宽 + 内边距（closable 额外加关闭区）。
    [[nodiscard]] auto tab_width(std::size_t i) const -> float {
        Font f;
        f.size_pt = m_font_size;
        float w = render::FontEngine::measure_width(m_tabs[i].label, f) + (m_tab_padding * 2.0f);
        if (m_tabs[i].closable) {
            w += m_aurora_close_zone;
        }
        return std::max(w, 60.0f);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<Tab> m_tabs;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> m_child_view;
    State<int> m_selected{ 0 };
    float m_tab_height = 36.0f;
    std::function<void(int)> m_on_change;
    std::function<void(int)> m_on_close;
    std::optional<Color> m_active_color;                  ///< 选中文本/指示器色；空 = 跟随主题 primary
    Color m_bar_background = Color{ 245, 245, 247, 255 }; ///< 标签栏背景色
    Color m_tab_background = Color{ 255, 255, 255, 255 }; ///< 选中标签背景色
    Color m_text_color = Color{ 96, 96, 96, 255 };        ///< 未选中标签文本色
    float m_indicator_thickness = 2.0f;                   ///< 选中指示器厚度 dp；0 = 不画
    float m_font_size = 13.0f;                            ///< 标签字号 pt
    float m_tab_padding = 12.0f;                          ///< 标签左右内边距 dp
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
