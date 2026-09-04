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
        : value_(std::move(value)), on_changed_(std::move(on_changed)) {}
    explicit Slider(Binding<double> binding, std::function<void(double)> on_changed = {})
        : binding_(std::move(binding)), value_(binding_.get()), on_changed_(std::move(on_changed)) {}

    auto set_range(double min, double max) -> Slider & {
        min_ = min;
        max_ = max;
        return *this;
    }
    auto set_on_changed(std::function<void(double)> cb) -> Slider & {
        on_changed_ = std::move(cb);
        return *this;
    }

    /// @brief 设置已填充轨道与滑块颜色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_active_color(Color c) -> Slider & {
        active_color_ = c;
        return *this;
    }

    /// @brief 设置未填充轨道颜色（链式）。
    auto set_inactive_color(Color c) -> Slider & {
        inactive_color_ = c;
        return *this;
    }

    /// @brief 设置滑块颜色（链式）。不调用则与激活色一致。
    auto set_thumb_color(Color c) -> Slider & {
        thumb_color_ = c;
        return *this;
    }

    /// @brief 设置轨道高度 dp（链式）。
    auto set_track_height(float h) -> Slider & {
        track_height_ = h > 0.0F ? h : 6.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置滑块直径 dp（链式；< 0 表示自动 = 控件高 − 6）。
    auto set_thumb_size(float d) -> Slider & {
        thumb_size_ = d;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置步进（链式；0 = 连续无级）。>0 时值吸附到 min + k×step 网格。
    auto set_step(double s) -> Slider & {
        step_ = s > 0.0 ? s : 0.0;
        return *this;
    }
    [[nodiscard]] auto step() const -> double { return step_; }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略拖拽。
    auto set_enabled(bool v) -> Slider & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    [[nodiscard]] auto value() const -> double { return binding_.bound() ? binding_.get() : value_.get(); }

    auto set_value(double v) -> void {
        double c = std::clamp(v, min_, max_);
        if (step_ > 0.0) {
            // 步进吸附：round 到最近网格点后再钳制（避免浮点越界）
            c = std::clamp(min_ + (std::round((c - min_) / step_) * step_), min_, max_);
        }
        if (binding_.bound()) {
            binding_.set(c);
        }
        value_ = c;
        if (on_changed_) {
            on_changed_(c);
        }
        mark_needs_paint();
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&value_);
        if (binding_.bound()) {
            out.push_back(binding_.target());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Slider"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Slider",
            .properties =
                {
                    {.name = "value",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "当前值",
                     .json_type = "number"},
                    {.name = "min",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "最小值",
                     .json_type = "number"},
                    {.name = "max",
                     .type = "double",
                     .default_value = "1.0",
                     .required = false,
                     .note = "最大值",
                     .json_type = "number"},
                    {.name = "step",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "步进；0=连续无级",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "已填充轨道与滑块颜色（缺省跟随主题 primary）",
                     .json_type = "array"},
                    {.name = "inactive_color",
                     .type = "Color",
                     .default_value = "{210,210,210,255}",
                     .required = false,
                     .note = "未填充轨道颜色",
                     .json_type = "array"},
                    {.name = "thumb_color",
                     .type = "Color",
                     .default_value = "active_color",
                     .required = false,
                     .note = "滑块颜色（缺省与激活色一致）",
                     .json_type = "array"},
                    {.name = "track_height",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "轨道高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "thumb_size",
                     .type = "float",
                     .default_value = "-1.0",
                     .required = false,
                     .note = "滑块直径(dp)；<0 自动=控件高−6",
                     .json_type = "number"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否可交互（禁用灰化）",
                     .json_type = "boolean"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {"on_changed"},
            .children_policy = "none",
            .invariants = {"min <= max", "value >= min", "value <= max"},
            .examples = {"au::Slider().set_range(0, 100)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉指针事件（不冒泡），但不改值
            return;
        }
        if (e.action == MouseAction::Press) {
            pressed_ = true;
            apply_at(e.local_position.x);
            e.is_handled = true;
        } else if (e.action == MouseAction::Move && pressed_) {
            apply_at(e.local_position.x);
            e.is_handled = true;
        } else if (e.action == MouseAction::Release) {
            pressed_ = false;
            mark_needs_paint();
            e.is_handled = true;
        }
    }

    /// @brief 悬停反馈：滑块调暗。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled_) {
            mark_needs_paint();
        }
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = value();
        props["min"] = min_;
        props["max"] = max_;
        if (step_ > 0.0) {
            props["step"] = step_;
        }
        if (active_color_.has_value()) {
            props["active_color"] = color_to_json(*active_color_);  // 未设置不输出：保留「跟随主题」语义
        }
        props["inactive_color"] = color_to_json(inactive_color_);
        if (thumb_color_.has_value()) {
            props["thumb_color"] = color_to_json(*thumb_color_);
        }
        props["track_height"] = track_height_;
        props["thumb_size"] = thumb_size_;
        props["enabled"] = enabled_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("min")) {
            min_ = props["min"].get<double>();
        }
        if (props.contains("max")) {
            max_ = props["max"].get<double>();
        }
        if (props.contains("step")) {
            step_ = props["step"].get<double>();
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(props["active_color"]);
        }
        if (props.contains("inactive_color")) {
            inactive_color_ = json_to_color(props["inactive_color"]);
        }
        if (props.contains("thumb_color")) {
            thumb_color_ = json_to_color(props["thumb_color"]);
        }
        if (props.contains("track_height")) {
            track_height_ = props["track_height"].get<float>();
        }
        if (props.contains("thumb_size")) {
            thumb_size_ = props["thumb_size"].get<float>();
        }
        if (props.contains("enabled")) {
            enabled_ = props["enabled"].get<bool>();
        }
        if (props.contains("value")) {
            set_value(props["value"].get<double>());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        constexpr float h = 24.0F;
        const float w = c.max.is_finite() ? c.max.width : 160.0F;
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color active = active_color_.value_or(inherit_theme(ctx).primary);
        Color inactive = inactive_color_;
        Color thumb = thumb_color_.value_or(active);
        if (!enabled_) {
            active = Color{176, 176, 180, 255};
            inactive = Color{225, 225, 228, 255};
            thumb = Color{200, 200, 204, 255};
        } else if (hovered() || pressed_) {
            thumb = thumb.shaded(pressed_ ? 0.78F : 0.90F);  // 悬停/拖动反馈
        }
        const Rect track = track_rect(bounds);
        paint_track(p, track, inactive);
        paint_active_track(p, track, active);
        paint_thumb(p, bounds, track, thumb);
    }

    // ---- 继承扩展点：几何与分阶段绘制 ----

    /// @brief 轨道矩形（水平居中，左右各留 4dp 内缩供滑块出头）。
    [[nodiscard]] auto track_rect(const Rect &bounds) const -> Rect {
        constexpr float inset = 4.0F;
        return Rect{.origin = Point{.x = bounds.origin.x + inset,
                                    .y = bounds.origin.y + ((bounds.size.height - track_height_) * 0.5F)},
                    .size = Size{.width = bounds.size.width - (2.0F * inset), .height = track_height_}};
    }

    /// @brief 当前值在 [min,max] 中的归一化占比 [0,1]。
    [[nodiscard]] auto value_fraction() const -> float {
        return (max_ > min_) ? static_cast<float>((value() - min_) / (max_ - min_)) : 0.0F;
    }

    /// @brief 绘制未填充轨道（圆角胶囊）。
    virtual auto paint_track(Painter &p, const Rect &track, Color c) -> void {
        p.fill_rounded_rect(track, track.size.height * 0.5F, c);
    }

    /// @brief 绘制已填充轨道（从左端到当前值）。
    virtual auto paint_active_track(Painter &p, const Rect &track, Color c) -> void {
        const float w = value_fraction() * track.size.width;
        if (w <= 0.0F) {
            return;
        }
        p.fill_rounded_rect(Rect{.origin = track.origin, .size = Size{.width = w, .height = track.size.height}},
                            track.size.height * 0.5F, c);
    }

    /// @brief 绘制圆形滑块（中心在当前值位置）。
    virtual auto paint_thumb(Painter &p, const Rect &bounds, const Rect &track, Color c) -> void {
        const float d = thumb_size_ > 0.0F ? thumb_size_ : bounds.size.height - 6.0F;
        const float cx = track.origin.x + (value_fraction() * track.size.width);
        const float cy = bounds.origin.y + (bounds.size.height * 0.5F);
        const Rect knob{.origin = Point{.x = cx - (d * 0.5F), .y = cy - (d * 0.5F)},
                        .size = Size{.width = d, .height = d}};
        p.fill_rounded_rect(knob, d * 0.5F, c);
    }

    auto apply_at(float local_x) -> void {
        if (size_.width <= 0.0F) {
            return;
        }
        constexpr float inset = 4.0F;
        const float track_w = size_.width - (2.0F * inset);
        const float local = local_x - inset;
        const float frac = std::clamp(local / track_w, 0.0F, 1.0F);
        set_value(min_ + (static_cast<double>(frac) * (max_ - min_)));
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<double> binding_;  // 声明须在 m_value 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<double> value_;
    std::function<void(double)> on_changed_;
    double min_ = 0.0;
    double max_ = 1.0;
    double step_ = 0.0;  ///< 步进；0 = 连续无级
    std::optional<Color> active_color_;  ///< 已填充轨道与滑块颜色；空 = 跟随主题 primary
    Color inactive_color_ = Color{210, 210, 210, 255};  ///< 未填充轨道颜色
    std::optional<Color> thumb_color_;  ///< 滑块颜色；空 = 与激活色一致
    float track_height_ = 6.0F;  ///< 轨道高度 dp
    float thumb_size_ = -1.0F;  ///< 滑块直径 dp；< 0 自动 = 控件高 − 6
    bool enabled_ = true;  ///< 禁用态灰化并忽略拖拽
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
