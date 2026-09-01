#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h" // inherit_theme：active_color 未显式设置时跟随主题 primary
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 分段控件。
 *
 * `SegmentedControl<T>{segments, selected, on_change}` — 互斥分段选择。
 *
 * 可定制性（对标 SwiftUI `Picker(.segmented)`、Flutter `SegmentedButton`）：
 * - `active_color`（选中段填充）未显式设置时跟随主题 `Theme::primary`；
 * - 文本色 / 选中文本色 / 边框色 / 字号 / 圆角均可配；
 * - 悬停段淡色反馈；禁用（`set_enabled(false)`）灰化并忽略点击。
 *
 * 继承扩展点（protected 虚函数）：`paint_segment`（单个分段）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class SegmentedControl : public LeafWidget {
  public:
    SegmentedControl() = default;
    explicit SegmentedControl(std::vector<std::string> segments, int selected = 0)
        : m_segments(std::move(segments)), m_selected(selected) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "SegmentedControl"; }
    [[nodiscard]] auto segments() const -> const std::vector<std::string> & { return m_segments; }
    [[nodiscard]] auto selected() const -> int { return m_selected; }
    auto set_selected(int i) -> SegmentedControl & {
        m_selected = i;
        mark_needs_paint();
        return *this;
    }
    auto set_on_change(std::function<void(int)> cb) -> SegmentedControl & {
        m_on_change = std::move(cb);
        return *this;
    }

    /// @brief 设置选中段填充色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> SegmentedControl & {
        m_active_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置未选中段文本色（链式）。
    auto set_text_color(Color c) -> SegmentedControl & {
        m_text_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置选中段文本色（链式）。
    auto set_selected_text_color(Color c) -> SegmentedControl & {
        m_selected_text_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置外框边框色（链式）。
    auto set_border_color(Color c) -> SegmentedControl & {
        m_border_color = c;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置字号 pt（链式）。
    auto set_font_size(float s) -> SegmentedControl & {
        m_font_size = s > 0.0f ? s : 14.0f;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置外框圆角半径 dp（链式；0 = 直角）。
    auto set_corner_radius(float r) -> SegmentedControl & {
        m_corner_radius = r >= 0.0f ? r : 0.0f;
        mark_needs_paint();
        return *this;
    }
    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> SegmentedControl & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "SegmentedControl",
            .properties = {
                { .name = "selected", .type = "int", .default_value = "0", .required = false, .note = "选中序号", .json_type = "integer", .enum_values = {}, .min_value = "0" },
                { .name = "active_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "选中段填充色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "text_color", .type = "Color", .default_value = "Color::black()", .required = false, .note = "未选中段文本色", .json_type = "array" },
                { .name = "selected_text_color", .type = "Color", .default_value = "Color::white()", .required = false, .note = "选中段文本色", .json_type = "array" },
                { .name = "border_color", .type = "Color", .default_value = "{200,200,200,255}", .required = false, .note = "外框边框色", .json_type = "array" },
                { .name = "font_size", .type = "float", .default_value = "14.0", .required = false, .note = "字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "corner_radius", .type = "float", .default_value = "6.0", .required = false, .note = "外框圆角半径(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_change" },
            .children_policy = "none",
            .examples = { R"(au::SegmentedControl({"Day", "Week", "Month"}, 0))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡），不切换
            return;
        }
        if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
            const Font f{ .size_pt = m_font_size };
            float x = 0.0f;
            for (size_t i = 0; i < m_segments.size(); ++i) {
                const float sw = render::FontEngine::measure_width(m_segments[i], f) + 24.0f;
                if (e.local_position.x >= x && e.local_position.x < x + sw) {
                    if (std::cmp_not_equal(m_selected, i)) {
                        m_selected = static_cast<int>(i);
                        if (m_on_change) {
                            m_on_change(m_selected);
                        }
                        mark_needs_paint();
                    }
                    break;
                }
                x += sw;
            }
            e.handled = true;
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["selected"] = m_selected;
        Json segs = Json::array();
        for (const auto &s : m_segments) {
            segs.push_back(s);
        }
        props["segments"] = segs;
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["text_color"] = color_to_json(m_text_color);
        props["selected_text_color"] = color_to_json(m_selected_text_color);
        props["border_color"] = color_to_json(m_border_color);
        props["font_size"] = m_font_size;
        props["corner_radius"] = m_corner_radius;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("selected")) {
            m_selected = props["selected"].get<int>();
        }
        if (props.contains("segments") && props["segments"].is_array()) {
            m_segments.clear();
            for (const auto &s : props["segments"]) {
                m_segments.push_back(s.get<std::string>());
            }
        }
        if (props.contains("active_color")) {
            m_active_color = json_to_color(props["active_color"]);
        }
        if (props.contains("text_color")) {
            m_text_color = json_to_color(props["text_color"]);
        }
        if (props.contains("selected_text_color")) {
            m_selected_text_color = json_to_color(props["selected_text_color"]);
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
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
        const Font f{ .size_pt = m_font_size };
        const float seg_h = render::FontEngine::measure_height(f) + 12.0f;
        float total_w = 0.0f;
        for (const auto &s : m_segments) {
            total_w += render::FontEngine::measure_width(s, f) + 24.0f;
        }
        return c.constrain(Size{ .width = total_w, .height = seg_h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Font f{ .size_pt = m_font_size };
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = m_active_color.value_or(inherit_theme(ctx).primary);
        Color text = m_text_color;
        Color sel_text = m_selected_text_color;
        Color border = m_border_color;
        if (!m_enabled) {
            accent = Color{ 176, 176, 180, 255 };
            text = Color{ 168, 168, 172, 255 };
            sel_text = Color{ 240, 240, 242, 255 };
            border = Color{ 215, 215, 219, 255 };
        }
        // 外框（圆角裁剪保证选中段填充不溢出圆角）
        if (m_corner_radius > 0.0f) {
            p.push_clip_rounded(bounds, m_corner_radius);
        }
        float x = bounds.origin.x;
        for (size_t i = 0; i < m_segments.size(); ++i) {
            const float sw = render::FontEngine::measure_width(m_segments[i], f) + 24.0f;
            const Rect seg{ .origin = Point{ .x = x, .y = bounds.origin.y },
                            .size = Size{ .width = sw, .height = bounds.size.height } };
            paint_segment(p, i, seg, std::cmp_equal(i, m_selected), f, accent, text, sel_text);
            x += sw;
        }
        if (m_corner_radius > 0.0f) {
            p.pop_clip();
        }
        if (m_corner_radius > 0.0f) {
            p.draw_rounded_border(bounds, m_corner_radius, 1.0f, border);
        } else {
            p.draw_rect(bounds, border);
        }
    }

    /// @brief 继承扩展点：绘制单个分段（选中填充强调色，未选中透明）。
    virtual auto paint_segment(Painter &p, size_t index, const Rect &seg, bool selected, const Font &f, Color accent,
                               Color text, Color sel_text) -> void {
        if (selected) {
            p.fill_rect(seg, accent);
        }
        p.draw_text(Rect{ .origin = Point{ .x = seg.origin.x + 12.0f, .y = seg.origin.y + 6.0f },
                          .size = Size{ .width = seg.size.width - 24.0f, .height = seg.size.height } },
                    m_segments[index], f, selected ? sel_text : text);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    std::vector<std::string> m_segments;
    int m_selected = 0;
    std::function<void(int)> m_on_change;
    std::optional<Color> m_active_color;                ///< 选中段填充色；空 = 跟随主题 primary
    Color m_text_color = Color::black();                ///< 未选中段文本色
    Color m_selected_text_color = Color::white();       ///< 选中段文本色
    Color m_border_color = Color{ 200, 200, 200, 255 }; ///< 外框边框色
    float m_font_size = 14.0f;                          ///< 字号 pt
    float m_corner_radius = 6.0f;                       ///< 外框圆角半径 dp；0 = 直角
    bool m_enabled = true;                              ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
