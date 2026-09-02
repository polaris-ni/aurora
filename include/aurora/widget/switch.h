#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 开关（叶控件）：布尔状态 `bool`，点击切换；绘制为圆角轨道 + 圆形滑块。
 *
 * 值来源与 `Checkbox` 一致（`Reactive<bool>` / `Binding<bool>` + `onChanged`）。
 *
 * 视觉（对标 Material 3 Switch / Fluent ToggleSwitch）：
 * - 开启：激活色轨道（`active_color` 未显式设置时跟随主题 `Theme::primary`）；
 * - 关闭：灰色轨道，可选描边（`set_border`，Material 3 关闭态轮廓样式）；
 * - 悬停/按下轨道调暗反馈；禁用（`set_enabled(false)`）灰化并忽略点击；
 * - 轨道尺寸（`set_track_size`）与滑块边距（`set_thumb_inset`）可调。
 *
 * 继承扩展点（protected 虚函数）：`paint_track` / `paint_thumb`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Switch : public LeafWidget {
  public:
    Switch() = default;
    explicit Switch(Reactive<bool> value, std::function<void(bool)> on_changed = {})
        : m_value(std::move(value)), m_on_changed(std::move(on_changed)) {}
    explicit Switch(Binding<bool> binding, std::function<void(bool)> on_changed = {})
        : m_binding(std::move(binding)), m_value(m_binding.get()), m_on_changed(std::move(on_changed)) {}

    auto set_on_changed(std::function<void(bool)> cb) -> Switch & {
        m_on_changed = std::move(cb);
        return *this;
    }

    /// @brief 设置开启态轨道色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> Switch & {
        m_active_color = c;
        return *this;
    }

    /// @brief 设置关闭态轨道色（链式）。
    auto set_inactive_color(Color c) -> Switch & {
        m_inactive_color = c;
        return *this;
    }

    /// @brief 设置滑块（圆形）颜色（链式；默认白色）。
    auto set_thumb_color(Color c) -> Switch & {
        m_thumb_color = c;
        return *this;
    }

    /// @brief 设置轨道尺寸 dp（链式；决定控件自然尺寸）。
    auto set_track_size(float w, float h) -> Switch & {
        m_track_width = w > 0.0f ? w : 44.0f;
        m_track_height = h > 0.0f ? h : 24.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置滑块与轨道边缘的间距 dp（链式）。
    auto set_thumb_inset(float inset) -> Switch & {
        m_thumb_inset = inset >= 0.0f ? inset : 2.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置关闭态轨道描边（链式；width<=0 不描边）。对标 Material 3 关闭态轮廓。
    auto set_border(Color c, float width = 1.5f) -> Switch & {
        m_border_color = c;
        m_border_width = width;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    auto set_enabled(bool v) -> Switch & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    [[nodiscard]] auto value() const -> bool { return m_binding.bound() ? m_binding.get() : m_value.get(); }

    auto set_value(bool v) -> void {
        if (m_binding.bound()) {
            m_binding.set(v);
        }
        m_value = v;
        if (m_on_changed) {
            m_on_changed(v);
        }
        mark_needs_paint();
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&m_value);
        if (m_binding.bound()) {
            out.push_back(m_binding.target());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Switch"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Switch",
            .properties = {
                { .name = "checked", .type = "bool", .default_value = "false", .required = false, .note = "开关状态", .json_type = "boolean" },
                { .name = "active_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "开启态轨道色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "inactive_color", .type = "Color", .default_value = "{180,180,180,255}", .required = false, .note = "关闭态轨道色", .json_type = "array" },
                { .name = "thumb_color", .type = "Color", .default_value = "Color::white()", .required = false, .note = "滑块颜色", .json_type = "array" },
                { .name = "track_width", .type = "float", .default_value = "44.0", .required = false, .note = "轨道宽(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "track_height", .type = "float", .default_value = "24.0", .required = false, .note = "轨道高(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "thumb_inset", .type = "float", .default_value = "2.0", .required = false, .note = "滑块与轨道边缘间距(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "border_color", .type = "Color", .default_value = "none", .required = false, .note = "关闭态轨道描边色（缺省不描边）", .json_type = "array", .enum_values = {}, .min_value = "", .max_value = "", .pattern = "", .constraint = "", .requires_props = {"border_width"} },
                { .name = "border_width", .type = "float", .default_value = "0.0", .required = false, .note = "关闭态描边宽(dp)；0=不描边", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_changed" },
            .children_policy = "none",
            .examples = { "au::Switch()" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉点击（不冒泡触发父级点击），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            m_pressed = true;
            mark_needs_paint(); // 按下态视觉反馈
            e.handled = true;
        } else if (e.action == MouseAction::Release) {
            if (m_pressed) {
                set_value(!value());
            }
            m_pressed = false;
            mark_needs_paint();
            e.handled = true;
        }
    }

    /// @brief 悬停反馈：轨道调暗。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (m_enabled) {
            mark_needs_paint();
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["checked"] = value();
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["inactive_color"] = color_to_json(m_inactive_color);
        props["thumb_color"] = color_to_json(m_thumb_color);
        props["track_width"] = m_track_width;
        props["track_height"] = m_track_height;
        props["thumb_inset"] = m_thumb_inset;
        if (m_border_color.has_value()) {
            props["border_color"] = color_to_json(*m_border_color);
        }
        if (m_border_width > 0.0f) {
            props["border_width"] = m_border_width;
        }
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("checked")) {
            set_value(props["checked"].get<bool>());
        }
        if (props.contains("active_color")) {
            m_active_color = json_to_color(props["active_color"]);
        }
        if (props.contains("inactive_color")) {
            m_inactive_color = json_to_color(props["inactive_color"]);
        }
        if (props.contains("thumb_color")) {
            m_thumb_color = json_to_color(props["thumb_color"]);
        }
        if (props.contains("track_width")) {
            m_track_width = props["track_width"].get<float>();
        }
        if (props.contains("track_height")) {
            m_track_height = props["track_height"].get<float>();
        }
        if (props.contains("thumb_inset")) {
            m_thumb_inset = props["thumb_inset"].get<float>();
        }
        if (props.contains("border_color")) {
            m_border_color = json_to_color(props["border_color"]);
        }
        if (props.contains("border_width")) {
            m_border_width = props["border_width"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
    }

  protected:
    // ---- 继承扩展点：分阶段绘制 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = m_track_width, .height = m_track_height });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const bool on = value();
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color active = m_active_color.value_or(inherit_theme(ctx).primary);
        Color inactive = m_inactive_color;
        Color thumb = m_thumb_color;
        if (!m_enabled) {
            active = Color{ 176, 176, 180, 255 };
            inactive = Color{ 210, 210, 214, 255 };
            thumb = Color{ 240, 240, 242, 255 };
        }
        Color track = on ? active : inactive;
        if (m_enabled && (hovered() || m_pressed)) {
            track = track.shaded(m_pressed ? 0.80f : 0.90f); // 悬停/按下反馈
        }
        paint_track(p, bounds, track, on);
        paint_thumb(p, bounds, thumb, on);
    }

    /// @brief 绘制圆角轨道（关闭态可选描边）。
    virtual auto paint_track(Painter &p, const Rect &bounds, Color track, bool on) -> void {
        const float radius = bounds.size.height * 0.5f;
        p.fill_rounded_rect(bounds, radius, track);
        if (!on && m_border_width > 0.0f && m_border_color.has_value()) {
            const Color bc = m_enabled ? *m_border_color : m_border_color->with_alpha(128);
            p.draw_rounded_border(bounds, radius, m_border_width, bc);
        }
    }

    /// @brief 绘制圆形滑块（开=右端，关=左端）。
    virtual auto paint_thumb(Painter &p, const Rect &bounds, Color thumb, bool on) -> void {
        const float d = bounds.size.height - (2.0f * m_thumb_inset);
        const float knob_x = on ? (bounds.right() - d - m_thumb_inset) : (bounds.origin.x + m_thumb_inset);
        const Rect knob{ .origin = Point{ .x = knob_x, .y = bounds.origin.y + m_thumb_inset },
                         .size = Size{ .width = d, .height = d } };
        p.fill_rounded_rect(knob, d * 0.5f, thumb);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<bool> m_binding; // 声明须在 m_value 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<bool> m_value;
    std::function<void(bool)> m_on_changed;
    std::optional<Color> m_active_color;                  ///< 开启态轨道色；空 = 跟随主题 primary
    Color m_inactive_color = Color{ 180, 180, 180, 255 }; ///< 关闭态轨道色
    Color m_thumb_color = colors::AURORA_WHITE;           ///< 滑块（圆形）颜色
    float m_track_width = 44.0f;                          ///< 轨道宽 dp
    float m_track_height = 24.0f;                         ///< 轨道高 dp
    float m_thumb_inset = 2.0f;                           ///< 滑块与轨道边缘间距 dp
    std::optional<Color> m_border_color;                  ///< 关闭态轨道描边色；空 = 不描边
    float m_border_width = 0.0f;                          ///< 关闭态描边宽 dp；0 = 不描边
    bool m_enabled = true;                                ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
