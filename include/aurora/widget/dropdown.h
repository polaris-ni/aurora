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
#include "aurora/theming/theme_scope.h" // inherit_theme：accent_color 未显式设置时跟随主题 primary
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 下拉选择器：点击展开选项列表，选择后收起。
 *
 * 选项为字符串列表（序列化友好、AI 可枚举）；泛型选项经 `on_change(index)`
 * 回调在调用侧映射。选中序号存于响应式 `selected()`。
 *
 * 可定制性（对标 Qt QComboBox / Flutter DropdownButton）：
 * - 颜色：主框背景/边框/文本/箭头；选中高亮 `accent_color` 未显式设置时跟随主题 `Theme::primary`；
 * - 尺寸：主框高（`set_box_height`）、选项行高（`set_item_height`）、字号、圆角；
 * - 空选项时显示 `placeholder`；禁用（`set_enabled(false)`）灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数）：`paint_box`（主框）与 `paint_item`（单个下拉选项行）。
 *
 * 对标 Qt `QComboBox`、WPF `ComboBox`、Flutter `DropdownButton`、SwiftUI `Picker`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Dropdown : public Widget {
  public:
    Dropdown() = default;
    explicit Dropdown(std::vector<std::string> options, int initial = 0) : m_options(std::move(options)) {
        const int max_idx = static_cast<int>(m_options.size()) - 1;
        m_selected.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Dropdown"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Dropdown",
            .properties = {
                { .name="options", .type="vector<string>", .default_value="[]", .required=true, .note="选项列表", .json_type="array" },
                { .name="selected_index", .type="int", .default_value="0", .required=false, .note="当前选中序号", .json_type="integer", .enum_values={}, .min_value="0" },
                { .name = "placeholder", .type = "string", .default_value = "\"\"", .required = false, .note = "空选项时的占位文本", .json_type = "string" },
                { .name = "accent_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "选中高亮色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "box_color", .type = "Color", .default_value = "Color::white()", .required = false, .note = "主框背景色", .json_type = "array" },
                { .name = "border_color", .type = "Color", .default_value = "{200,200,205,255}", .required = false, .note = "主框/面板边框色", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "{30,30,30,255}", .required = false, .note = "选项文本色", .json_type = "array" },
                { .name = "arrow_color", .type = "Color", .default_value = "{120,120,125,255}", .required = false, .note = "箭头颜色", .json_type = "array" },
                { .name = "box_height", .type = "float", .default_value = "30.0", .required = false, .note = "主框高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "item_height", .type = "float", .default_value = "26.0", .required = false, .note = "选项行高(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "font_size", .type = "float", .default_value = "13.0", .required = false, .note = "字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "corner_radius", .type = "float", .default_value = "4.0", .required = false, .note = "主框圆角半径(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .invariants = { "selected_index >= 0", "selected_index < options.size()" },
            .examples = { R"(au::Dropdown({"Small", "Medium", "Large"}, 1))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_selected); }

    [[nodiscard]] auto option_count() const -> std::size_t { return m_options.size(); }
    [[nodiscard]] auto selected() -> State<int> & { return m_selected; }
    [[nodiscard]] auto selected_index() const -> int { return m_selected.get(); }
    [[nodiscard]] auto selected_text() const -> std::string {
        const int i = m_selected.get();
        if (i < 0 || std::cmp_greater_equal(i, m_options.size())) {
            return {};
        }
        return m_options[static_cast<std::size_t>(i)];
    }
    [[nodiscard]] auto is_open() const -> bool { return m_open; }

    /// @brief 选择指定序号（越界忽略；触发 on_change）。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, m_options.size()) && index != m_selected.get()) {
            m_selected.set(index);
            mark_needs_paint();
            if (m_on_change) {
                m_on_change(index);
            }
        }
    }

    /// @brief 展开/收起下拉。
    auto set_open(bool open) -> void {
        m_open = open;
        mark_needs_paint();
    }

    /// @brief 设置选择回调（链式）。
    auto set_on_change(std::function<void(int)> cb) -> Dropdown & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 设置空选项占位文本（链式）。
    auto set_placeholder(std::string s) -> Dropdown & {
        m_placeholder = std::move(s);
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中高亮色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_accent_color(Color c) -> Dropdown & {
        m_accent_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框背景色（链式）。
    auto set_box_color(Color c) -> Dropdown & {
        m_box_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框/面板边框色（链式）。
    auto set_border_color(Color c) -> Dropdown & {
        m_border_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选项文本色（链式）。
    auto set_text_color(Color c) -> Dropdown & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置箭头颜色（链式）。
    auto set_arrow_color(Color c) -> Dropdown & {
        m_arrow_color = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框高度 dp（链式）。
    auto set_box_height(float h) -> Dropdown & {
        m_box_height = h > 0.0f ? h : 30.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置选项行高 dp（链式）。
    auto set_item_height(float h) -> Dropdown & {
        m_item_height = h > 0.0f ? h : 26.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置字号 pt（链式）。
    auto set_font_size(float s) -> Dropdown & {
        m_font_size = s > 0.0f ? s : 13.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置主框圆角半径 dp（链式；0 = 直角）。
    auto set_corner_radius(float r) -> Dropdown & {
        m_corner_radius = r >= 0.0f ? r : 0.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> Dropdown & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    /// @brief 点击交互：框内点击开合；展开时点击选项选中并收起。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡），不开合
            return;
        }
        if (e.action != MouseAction::Press) {
            Widget::on_pointer_event(e);
            return;
        }
        // 主框区域：开合切换
        if (e.local_position.y < m_box_height) {
            m_open = !m_open;
            mark_needs_paint();
            e.handled = true;
            return;
        }
        // 展开中的选项列表
        if (m_open) {
            const int idx = static_cast<int>((e.local_position.y - m_box_height) / m_item_height);
            if (idx >= 0 && std::cmp_less(idx, m_options.size())) {
                select(idx);
            }
            m_open = false;
            mark_needs_paint();
            e.handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停反馈：主框边框高亮为强调色。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (m_enabled) {
            mark_needs_paint();
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        Json opts = Json::array();
        for (const auto &o : m_options) {
            opts.push_back(o);
        }
        props["options"] = opts;
        props["selected_index"] = m_selected.get();
        if (!m_placeholder.empty()) {
            props["placeholder"] = m_placeholder;
        }
        if (m_accent_color.has_value()) {
            props["accent_color"] = color_to_json(*m_accent_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["box_color"] = color_to_json(m_box_color);
        props["border_color"] = color_to_json(m_border_color);
        props["text_color"] = color_to_json(m_text_color);
        props["arrow_color"] = color_to_json(m_arrow_color);
        props["box_height"] = m_box_height;
        props["item_height"] = m_item_height;
        props["font_size"] = m_font_size;
        props["corner_radius"] = m_corner_radius;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("options") && props["options"].is_array()) {
            m_options.clear();
            for (const auto &o : props["options"]) {
                m_options.push_back(o.get<std::string>());
            }
        }
        if (props.contains("selected_index")) {
            m_selected.set(props["selected_index"].get<int>());
        }
        if (props.contains("placeholder")) {
            m_placeholder = props["placeholder"].get<std::string>();
        }
        if (props.contains("accent_color")) {
            m_accent_color = json_to_color(props["accent_color"]);
        }
        if (props.contains("box_color")) {
            m_box_color = json_to_color(props["box_color"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("arrow_color")) {
            m_arrow_color = json_to_color(props["arrow_color"]);
        }
        if (props.contains("box_height")) {
            m_box_height = props["box_height"].get<float>();
        }
        if (props.contains("item_height")) {
            m_item_height = props["item_height"].get<float>();
        }
        if (props.contains("font_size")) {
            m_font_size = props["font_size"].get<float>();
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        // 主框宽度 = 最长选项宽 + 箭头区；下拉为覆盖绘制不占布局
        Font f;
        f.size_pt = m_font_size;
        float w = 80.0f;
        for (const auto &o : m_options) {
            w = std::max(w, render::FontEngine::measure_width(o, f) + (m_aurora_pad * 2.0f) + m_aurora_arrow_zone);
        }
        if (m_options.empty() && !m_placeholder.empty()) {
            w = std::max(w, render::FontEngine::measure_width(m_placeholder, f) + (m_aurora_pad * 2.0f) +
                                m_aurora_arrow_zone);
        }
        return c.constrain(Size{ .width = w, .height = m_box_height });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = m_font_size;
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = m_accent_color.value_or(inherit_theme(ctx).primary);
        Color box = m_box_color;
        Color border = m_border_color;
        Color text = m_text_color;
        Color arrow = m_arrow_color;
        if (!m_enabled) {
            accent = Color{ 176, 176, 180, 255 };
            box = Color{ 235, 235, 237, 255 };
            border = Color{ 215, 215, 219, 255 };
            text = Color{ 168, 168, 172, 255 };
            arrow = Color{ 190, 190, 194, 255 };
        } else if (hovered() || m_open) {
            border = accent; // 悬停/展开时边框高亮
        }

        const Rect box_rect{ .origin = bounds.origin,
                             .size = Size{ .width = bounds.size.width, .height = m_box_height } };
        paint_box(p, box_rect, f, box, border, text, arrow);

        // 下拉选项面板
        if (m_open) {
            const float h = static_cast<float>(m_options.size()) * m_item_height;
            const Rect drop{ .origin = Point{ .x = box_rect.origin.x, .y = box_rect.origin.y + m_box_height },
                             .size = Size{ .width = box_rect.size.width, .height = h } };
            p.draw_shadow(drop, 0.0f, 2.0f, 8.0f, Color(0, 0, 0, 48));
            p.fill_rect(drop, m_box_color);
            p.draw_rect(drop, m_border_color);
            float y = drop.origin.y;
            for (std::size_t i = 0; i < m_options.size(); ++i) {
                const Rect item{ .origin = Point{ .x = drop.origin.x, .y = y },
                                 .size = Size{ .width = drop.size.width, .height = m_item_height } };
                paint_item(p, i, item, std::cmp_equal(i, m_selected.get()), f, accent, text);
                y += m_item_height;
            }
        }
    }

    /// @brief 继承扩展点：绘制主框（背景/边框/选中文本/箭头）。
    virtual auto paint_box(Painter &p, const Rect &box, const Font &f, Color bg, Color border, Color text, Color arrow)
        -> void {
        if (m_corner_radius > 0.0f) {
            p.fill_rounded_rect(box, m_corner_radius, bg);
            p.draw_rounded_border(box, m_corner_radius, 1.0f, border);
        } else {
            p.fill_rect(box, bg);
            p.draw_rect(box, border);
        }
        const std::string shown = m_options.empty() ? m_placeholder : selected_text();
        const Rect text_box{
            .origin = Point{ .x = box.origin.x + m_aurora_pad, .y = box.origin.y + ((box.size.height - 14.0f) * 0.5f) },
            .size = Size{ .width = box.size.width - (m_aurora_pad * 2.0f) - m_aurora_arrow_zone, .height = 14.0f }
        };
        p.draw_text(text_box, shown, f, text);
        // 箭头（简化为 "v"/"^"）
        const Rect arrow_box{ .origin = Point{ .x = box.origin.x + box.size.width - m_aurora_arrow_zone,
                                               .y = box.origin.y + ((box.size.height - 14.0f) * 0.5f) },
                              .size = Size{ .width = m_aurora_arrow_zone - 4.0f, .height = 14.0f } };
        p.draw_text(arrow_box, m_open ? "^" : "v", f, arrow);
    }

    /// @brief 继承扩展点：绘制单个下拉选项行（选中项铺强调色淡底 + 强调色文本）。
    virtual auto paint_item(Painter &p, std::size_t index, const Rect &item, bool selected, const Font &f, Color accent,
                            Color text) -> void {
        if (selected) {
            p.fill_rect(item, accent.with_alpha(30));
        }
        const Rect item_box{ .origin = Point{ .x = item.origin.x + m_aurora_pad, .y = item.origin.y + 5.0f },
                             .size = Size{ .width = item.size.width - (m_aurora_pad * 2.0f),
                                           .height = item.size.height - 10.0f } };
        p.draw_text(item_box, m_options[index], f, selected ? accent : text);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 主框
        if (local.y < m_box_height && Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                            .size = Size{ .width = bounds.size.width, .height = m_box_height } }
                                          .contains(local)) {
            return this;
        }
        // 展开的下拉区
        if (m_open) {
            const float h = static_cast<float>(m_options.size()) * m_item_height;
            const Rect drop{ .origin = Point{ .x = 0.0f, .y = m_box_height },
                             .size = Size{ .width = bounds.size.width, .height = h } };
            if (drop.contains(local)) {
                return this;
            }
        }
        return nullptr;
    }

    static constexpr float m_aurora_pad = 10.0f;        ///< 文本内边距(dp)
    static constexpr float m_aurora_arrow_zone = 24.0f; ///< 箭头区宽度(dp)

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> m_options;
    State<int> m_selected{ 0 };
    bool m_open = false;
    std::function<void(int)> m_on_change;
    std::string m_placeholder;                          ///< 空选项时的占位文本
    std::optional<Color> m_accent_color;                ///< 选中高亮色；空 = 跟随主题 primary
    Color m_box_color = Color{ 255, 255, 255, 255 };    ///< 主框背景色
    Color m_border_color = Color{ 200, 200, 205, 255 }; ///< 主框/面板边框色
    Color m_text_color = Color{ 30, 30, 30, 255 };      ///< 选项文本色
    Color m_arrow_color = Color{ 120, 120, 125, 255 };  ///< 箭头颜色
    float m_box_height = 30.0f;                         ///< 主框高度 dp
    float m_item_height = 26.0f;                        ///< 选项行高 dp
    float m_font_size = 13.0f;                          ///< 字号 pt
    float m_corner_radius = 4.0f;                       ///< 主框圆角半径 dp；0 = 直角
    bool m_enabled = true;                              ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
