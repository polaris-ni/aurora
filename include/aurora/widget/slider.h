#pragma once

#include <algorithm>
#include <cmath>
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
 * @brief 滑块（叶控件）：连续值 `double`（默认 [0,1]），拖拽设置。
 *
 * 值来源与 `Checkbox` 一致（`Reactive<double>` / `Binding<double>` + `onChanged`）。
 * 指针按下并拖动时，按局部 x 位置映射到 [min,max]；松手结束。绘制为圆角轨道 + 圆形滑块。
 *
 * 视觉（对标 Material 3 Slider / Qt QSlider）：
 * - `active_color` 未显式设置时跟随主题 `Theme::primary`（ThemeScope 换肤即生效）；
 * - 悬停/拖动时滑块调暗反馈；禁用（`set_enabled(false)`）灰化并忽略拖拽；
 * - `step > 0` 时值吸附到步进网格（对标 Qt `singleStep` / Flutter `divisions`）。
 *
 * 继承扩展点（protected 虚函数）：`paint_track` / `paint_active_track` / `paint_thumb`，
 * 几何由 `track_rect` / `value_fraction` 提供，子类可单独覆盖某个绘制阶段。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Slider : public LeafWidget {
  public:
    Slider() = default;
    explicit Slider(Reactive<double> value, std::function<void(double)> on_changed = {})
        : m_value(std::move(value)), m_on_changed(std::move(on_changed)) {}
    explicit Slider(Binding<double> binding, std::function<void(double)> on_changed = {})
        : m_binding(std::move(binding)), m_value(m_binding.get()), m_on_changed(std::move(on_changed)) {}

    auto set_range(double min, double max) -> Slider & {
        m_min = min;
        m_max = max;
        return *this;
    }
    auto set_on_changed(std::function<void(double)> cb) -> Slider & {
        m_on_changed = std::move(cb);
        return *this;
    }

    /// @brief 设置已填充轨道与滑块颜色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> Slider & {
        m_active_color = c;
        return *this;
    }

    /// @brief 设置未填充轨道颜色（链式）。
    auto set_inactive_color(Color c) -> Slider & {
        m_inactive_color = c;
        return *this;
    }

    /// @brief 设置滑块颜色（链式）。不调用则与激活色一致。
    auto set_thumb_color(Color c) -> Slider & {
        m_thumb_color = c;
        return *this;
    }

    /// @brief 设置轨道高度 dp（链式）。
    auto set_track_height(float h) -> Slider & {
        m_track_height = h > 0.0f ? h : 6.0f;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置滑块直径 dp（链式；< 0 表示自动 = 控件高 − 6）。
    auto set_thumb_size(float d) -> Slider & {
        m_thumb_size = d;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置步进（链式；0 = 连续无级）。>0 时值吸附到 min + k×step 网格。
    auto set_step(double s) -> Slider & {
        m_step = s > 0.0 ? s : 0.0;
        return *this;
    }
    [[nodiscard]] auto step() const -> double { return m_step; }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略拖拽。
    auto set_enabled(bool v) -> Slider & {
        m_enabled = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return m_enabled; }

    [[nodiscard]] auto value() const -> double { return m_binding.bound() ? m_binding.get() : m_value.get(); }

    auto set_value(double v) -> void {
        double c = std::clamp(v, m_min, m_max);
        if (m_step > 0.0) {
            // 步进吸附：round 到最近网格点后再钳制（避免浮点越界）
            c = std::clamp(m_min + (std::round((c - m_min) / m_step) * m_step), m_min, m_max);
        }
        if (m_binding.bound()) {
            m_binding.set(c);
        }
        m_value = c;
        if (m_on_changed) {
            m_on_changed(c);
        }
        mark_needs_paint();
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&m_value);
        if (m_binding.bound()) {
            out.push_back(m_binding.target());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Slider"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Slider",
            .properties = {
                { .name = "value", .type = "double", .default_value = "0.0", .required = false, .note = "当前值", .json_type = "number" },
                { .name = "min", .type = "double", .default_value = "0.0", .required = false, .note = "最小值", .json_type = "number" },
                { .name = "max", .type = "double", .default_value = "1.0", .required = false, .note = "最大值", .json_type = "number" },
                { .name = "step", .type = "double", .default_value = "0.0", .required = false, .note = "步进；0=连续无级", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "active_color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "已填充轨道与滑块颜色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "inactive_color", .type = "Color", .default_value = "{210,210,210,255}", .required = false, .note = "未填充轨道颜色", .json_type = "array" },
                { .name = "thumb_color", .type = "Color", .default_value = "active_color", .required = false, .note = "滑块颜色（缺省与激活色一致）", .json_type = "array" },
                { .name = "track_height", .type = "float", .default_value = "6.0", .required = false, .note = "轨道高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "thumb_size", .type = "float", .default_value = "-1.0", .required = false, .note = "滑块直径(dp)；<0 自动=控件高−6", .json_type = "number" },
                { .name = "enabled", .type = "bool", .default_value = "true", .required = false, .note = "是否可交互（禁用灰化）", .json_type = "boolean" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = { "on_changed" },
            .children_policy = "none",
            .invariants = { "min <= max", "value >= min", "value <= max" },
            .examples = { "au::Slider().set_range(0, 100)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_enabled) {
            e.handled = true; // 禁用态吞掉指针事件（不冒泡），但不改值
            return;
        }
        if (e.action == MouseAction::Press) {
            m_pressed = true;
            apply_at(e.local_position.x);
            e.handled = true;
        } else if (e.action == MouseAction::Move && m_pressed) {
            apply_at(e.local_position.x);
            e.handled = true;
        } else if (e.action == MouseAction::Release) {
            m_pressed = false;
            mark_needs_paint();
            e.handled = true;
        }
    }

    /// @brief 悬停反馈：滑块调暗。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (m_enabled) {
            mark_needs_paint();
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = value();
        props["min"] = m_min;
        props["max"] = m_max;
        if (m_step > 0.0) {
            props["step"] = m_step;
        }
        if (m_active_color.has_value()) {
            props["active_color"] = color_to_json(*m_active_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["inactive_color"] = color_to_json(m_inactive_color);
        if (m_thumb_color.has_value()) {
            props["thumb_color"] = color_to_json(*m_thumb_color);
        }
        props["track_height"] = m_track_height;
        props["thumb_size"] = m_thumb_size;
        props["enabled"] = m_enabled;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("min")) {
            m_min = props["min"].get<double>();
        }
        if (props.contains("max")) {
            m_max = props["max"].get<double>();
        }
        if (props.contains("step")) {
            m_step = props["step"].get<double>();
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
        if (props.contains("track_height")) {
            m_track_height = props["track_height"].get<float>();
        }
        if (props.contains("thumb_size")) {
            m_thumb_size = props["thumb_size"].get<float>();
        }
        if (props.contains("enabled")) {
            m_enabled = props["enabled"].get<bool>();
        }
        if (props.contains("value")) {
            set_value(props["value"].get<double>());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        constexpr float h = 24.0f;
        const float w = c.max.is_finite() ? c.max.width : 160.0f;
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color active = m_active_color.value_or(inherit_theme(ctx).primary);
        Color inactive = m_inactive_color;
        Color thumb = m_thumb_color.value_or(active);
        if (!m_enabled) {
            active = Color{ 176, 176, 180, 255 };
            inactive = Color{ 225, 225, 228, 255 };
            thumb = Color{ 200, 200, 204, 255 };
        } else if (hovered() || m_pressed) {
            thumb = thumb.shaded(m_pressed ? 0.78f : 0.90f); // 悬停/拖动反馈
        }
        const Rect track = track_rect(bounds);
        paint_track(p, track, inactive);
        paint_active_track(p, track, active);
        paint_thumb(p, bounds, track, thumb);
    }

    // ---- 继承扩展点：几何与分阶段绘制 ----

    /// @brief 轨道矩形（水平居中，左右各留 4dp 内缩供滑块出头）。
    [[nodiscard]] auto track_rect(const Rect &bounds) const -> Rect {
        constexpr float inset = 4.0f;
        return Rect{ .origin = Point{ .x = bounds.origin.x + inset,
                                      .y = bounds.origin.y + ((bounds.size.height - m_track_height) * 0.5f) },
                     .size = Size{ .width = bounds.size.width - (2.0f * inset), .height = m_track_height } };
    }

    /// @brief 当前值在 [min,max] 中的归一化占比 [0,1]。
    [[nodiscard]] auto value_fraction() const -> float {
        return (m_max > m_min) ? static_cast<float>((value() - m_min) / (m_max - m_min)) : 0.0f;
    }

    /// @brief 绘制未填充轨道（圆角胶囊）。
    virtual auto paint_track(Painter &p, const Rect &track, Color c) -> void {
        p.fill_rounded_rect(track, track.size.height * 0.5f, c);
    }

    /// @brief 绘制已填充轨道（从左端到当前值）。
    virtual auto paint_active_track(Painter &p, const Rect &track, Color c) -> void {
        const float w = value_fraction() * track.size.width;
        if (w <= 0.0f) {
            return;
        }
        p.fill_rounded_rect(Rect{ .origin = track.origin, .size = Size{ .width = w, .height = track.size.height } },
                            track.size.height * 0.5f, c);
    }

    /// @brief 绘制圆形滑块（中心在当前值位置）。
    virtual auto paint_thumb(Painter &p, const Rect &bounds, const Rect &track, Color c) -> void {
        const float d = m_thumb_size > 0.0f ? m_thumb_size : bounds.size.height - 6.0f;
        const float cx = track.origin.x + (value_fraction() * track.size.width);
        const float cy = bounds.origin.y + (bounds.size.height * 0.5f);
        const Rect knob{ .origin = Point{ .x = cx - (d * 0.5f), .y = cy - (d * 0.5f) },
                         .size = Size{ .width = d, .height = d } };
        p.fill_rounded_rect(knob, d * 0.5f, c);
    }

    auto apply_at(float local_x) -> void {
        if (m_size.width <= 0.0f) {
            return;
        }
        constexpr float inset = 4.0f;
        const float track_w = m_size.width - (2.0f * inset);
        const float local = local_x - inset;
        const float frac = std::clamp(local / track_w, 0.0f, 1.0f);
        set_value(m_min + (static_cast<double>(frac) * (m_max - m_min)));
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<double> m_binding; // 声明须在 m_value 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<double> m_value;
    std::function<void(double)> m_on_changed;
    double m_min = 0.0;
    double m_max = 1.0;
    double m_step = 0.0;                                  ///< 步进；0 = 连续无级
    std::optional<Color> m_active_color;                  ///< 已填充轨道与滑块颜色；空 = 跟随主题 primary
    Color m_inactive_color = Color{ 210, 210, 210, 255 }; ///< 未填充轨道颜色
    std::optional<Color> m_thumb_color;                   ///< 滑块颜色；空 = 与激活色一致
    float m_track_height = 6.0f;                          ///< 轨道高度 dp
    float m_thumb_size = -1.0f;                           ///< 滑块直径 dp；< 0 自动 = 控件高 − 6
    bool m_enabled = true;                                ///< 禁用态灰化并忽略拖拽
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
