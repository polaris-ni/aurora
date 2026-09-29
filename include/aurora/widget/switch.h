/// @brief 开关控件 Switch 的声明：布尔状态叶控件，点击切换，圆角轨道 + 圆形滑块。
/// @file switch.h
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 开关（叶控件）：布尔状态 `bool`，点击切换；绘制为圆角轨道 + 圆形滑块。
///
/// 值来源与 `Checkbox` 一致（`Reactive<bool>` / `Binding<bool>` + `onChanged`）。
///
/// 视觉（对标 Material 3 Switch / Fluent ToggleSwitch）：
/// - 开启：激活色轨道（`active_color` 未显式设置时跟随主题 `Theme::primary`）；
/// - 关闭：灰色轨道，可选描边（`set_border`，Material 3 关闭态轮廓样式）；
/// - 悬停/按下轨道调暗反馈；禁用（`set_enabled(false)`）灰化并忽略点击；
/// - 轨道尺寸（`set_track_size`）与滑块边距（`set_thumb_inset`）可调。
///
/// 继承扩展点（protected 虚函数）：`paint_track` / `paint_thumb`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Switch : public LeafWidget {
  public:
    Switch() = default;

    /// @brief 以响应式布尔值构造开关；on_changed 非空时在每次值变化后回调。
    /// @param value 初始状态与响应式数据源
    /// @param on_changed 值变化回调（可空）
    explicit Switch(Reactive<bool> value, std::function<void(bool)> on_changed = {})
        : value_(std::move(value)), on_changed_(std::move(on_changed)) {}

    /// @brief 以绑定 `Binding<bool>` 构造开关，初值取自 `binding.get()`，读写均经由该绑定。
    /// @param binding 布尔状态绑定源
    /// @param on_changed 值变化回调（可空）
    explicit Switch(Binding<bool> binding, std::function<void(bool)> on_changed = {})
        : binding_(std::move(binding)), value_(binding_.get()), on_changed_(std::move(on_changed)) {}

    /// @brief 替换值变化回调（链式）。
    /// @param cb 新回调（可空，空则切换时不回调）
    /// @return *this，便于链式调用
    auto set_on_changed(std::function<void(bool)> cb) -> Switch & {
        on_changed_ = std::move(cb);
        return *this;
    }

    /// @brief 设置开启态轨道色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c 开启态轨道颜色
    /// @return *this，便于链式调用
    auto set_active_color(Color c) -> Switch & {
        active_color_ = c;
        return *this;
    }

    /// @brief 设置关闭态轨道色（链式）。
    /// @param c 关闭态轨道颜色
    /// @return *this，便于链式调用
    auto set_inactive_color(Color c) -> Switch & {
        inactive_color_ = c;
        return *this;
    }

    /// @brief 设置滑块（圆形）颜色（链式；默认白色）。
    /// @param c 滑块颜色
    /// @return *this，便于链式调用
    auto set_thumb_color(Color c) -> Switch & {
        thumb_color_ = c;
        return *this;
    }

    /// @brief 设置轨道尺寸 dp（链式；决定控件自然尺寸）。
    /// @param w 轨道宽 dp，<=0 时回落为 44.0
    /// @param h 轨道高 dp，<=0 时回落为 24.0
    /// @return *this，便于链式调用
    auto set_track_size(float w, float h) -> Switch & {
        track_width_ = w > 0.0F ? w : 44.0F;
        track_height_ = h > 0.0F ? h : 24.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置滑块与轨道边缘的间距 dp（链式）。
    /// @param inset 间距 dp，<0 时回落为 2.0
    /// @return *this，便于链式调用
    auto set_thumb_inset(float inset) -> Switch & {
        thumb_inset_ = inset >= 0.0F ? inset : 2.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置关闭态轨道描边（链式；width<=0 不描边）。对标 Material 3 关闭态轮廓。
    /// @param c 描边颜色
    /// @param width 描边宽 dp（默认 1.5，<=0 视为不描边）
    /// @return *this，便于链式调用
    auto set_border(Color c, float width = 1.5F) -> Switch & {
        border_color_ = c;
        border_width_ = width;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    /// @param v 是否启用
    /// @return *this，便于链式调用
    auto set_enabled(bool v) -> Switch & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }

    /// @brief 读取启用状态。
    /// @return 当前是否启用
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 读取当前开关态：已绑定时读绑定值，否则读内部响应值。
    /// @return 当前布尔状态
    [[nodiscard]] auto value() const -> bool { return binding_.bound() ? binding_.get() : value_.get(); }

    /// @brief 设置开关态：写入绑定/响应值，触发 on_changed，请求重绘并派发无障碍 ValueChanged 事件。
    /// @param v 新状态
    auto set_value(bool v) -> void {
        if (binding_.bound()) {
            binding_.set(v);
        }
        value_ = v;
        if (on_changed_) {
            on_changed_(v);
        }
        mark_needs_paint();
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    /// @brief 收集本控件订阅的信号：内部 `value_`；已绑定时追加绑定目标。
    /// @param out 输出数组，信号视图指针追加到尾部
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&value_);
        if (binding_.bound()) {
            out.push_back(binding_.target());
        }
    }

    /// @brief 控件类型名（自描述）。
    /// @return 字面量 "Switch"
    [[nodiscard]] auto type_name() const -> const char * override { return "Switch"; }

    /// @brief 无障碍值：开关态的字面布尔串（`true` / `false`）。
    /// @return 当前开关态对应的字面串 "true" 或 "false"
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override { return value() ? "true" : "false"; }

    /// @brief 无障碍状态：开关语义两位（checkable 恒 true、checked 取当前值）。
    /// @return 置好 checkable/checked 的无障碍状态
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_state() const -> AccessibilityState override {
        AccessibilityState s = Widget::accessibility_state();
        s.checkable = true;
        s.checked = value();
        return s;
    }

    /// @brief 读屏 Toggle 动作：翻转开关态（走 `set_value` 既有路径）。
    /// @param req 动作请求，按 req.action 比对 Toggle
    /// @return 消费了 Toggle 动作时为 true，否则回落基类返回值
    /// @note Side-effects: mutates state
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Toggle) {
            set_value(!value());
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Switch 的属性 / 事件 / 示例描述符
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Switch",
            .properties =
                {
                    {.name = "checked",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Switch state",
                     .json_type = "boolean"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "Track color when on (defaults to theme primary)",
                     .json_type = "array"},
                    {.name = "inactive_color",
                     .type = "Color",
                     .default_value = "{180,180,180,255}",
                     .required = false,
                     .note = "Track color when off",
                     .json_type = "array"},
                    {.name = "thumb_color",
                     .type = "Color",
                     .default_value = "Color::white()",
                     .required = false,
                     .note = "Thumb color",
                     .json_type = "array"},
                    {.name = "track_width",
                     .type = "float",
                     .default_value = "44.0",
                     .required = false,
                     .note = "Track width (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "track_height",
                     .type = "float",
                     .default_value = "24.0",
                     .required = false,
                     .note = "Track height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "thumb_inset",
                     .type = "float",
                     .default_value = "2.0",
                     .required = false,
                     .note = "Gap between the thumb and the track edge (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "none",
                     .required = false,
                     .note = "Track outline color when off (no outline by default)",
                     .json_type = "array",
                     .enum_values = {},
                     .min_value = "",
                     .max_value = "",
                     .pattern = "",
                     .constraint = "",
                     .requires_props = {"border_width"}},
                    {.name = "border_width",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Outline width when off (dp); 0 = no outline",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "enabled",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Interactive (greyed out when disabled)",
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
            .examples = {"au::Switch()"},
        };
    }

    /// @brief 实例自描述：转发 `describe_static()`。
    /// @return Switch 的控件描述符
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 指针事件：禁用态吞掉点击（不冒泡、不切换）；按下置按下态，抬起于按下态下翻转开关。
    /// @param e 鼠标事件，处理后写回 e.is_handled
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡触发父级点击），但不切换
            return;
        }
        if (e.action == MouseAction::Press) {
            pressed_ = true;
            mark_needs_paint();  // 按下态视觉反馈
            e.is_handled = true;
        } else if (e.action == MouseAction::Release) {
            if (pressed_) {
                set_value(!value());
            }
            pressed_ = false;
            mark_needs_paint();
            e.is_handled = true;
        }
    }

    /// @brief 悬停反馈：轨道调暗。
    /// @param entered 是否进入悬停（先转发基类维护悬停态）
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled_) {
            mark_needs_paint();
        }
    }

    /// @brief 序列化开关属性；active_color/border_color/border_width 仅在显式设置时输出，保留「跟随主题」语义。
    /// @param props 输出的 JSON 对象，属性键值追加到其上
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("checked", Json{value()});
        if (active_color_.has_value()) {
            props.set("active_color", color_to_json(*active_color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("inactive_color", color_to_json(inactive_color_));
        props.set("thumb_color", color_to_json(thumb_color_));
        props.set("track_width", track_width_);
        props.set("track_height", track_height_);
        props.set("thumb_inset", thumb_inset_);
        if (border_color_.has_value()) {
            props.set("border_color", color_to_json(*border_color_));
        }
        if (border_width_ > 0.0F) {
            props.set("border_width", border_width_);
        }
        props.set("enabled", Json{enabled_});
    }

    /// @brief 反序列化开关属性：按键存在读取 checked/各颜色/轨道尺寸/边距/描边/enabled。
    /// @param props 输入的 JSON 对象
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("checked")) {
            set_value(props.at("checked")->as_or<bool>(false));
        }
        if (props.contains("active_color")) {
            active_color_ = json_to_color(*props.at("active_color"));
        }
        if (props.contains("inactive_color")) {
            inactive_color_ = json_to_color(*props.at("inactive_color"));
        }
        if (props.contains("thumb_color")) {
            thumb_color_ = json_to_color(*props.at("thumb_color"));
        }
        if (props.contains("track_width")) {
            track_width_ = props.at("track_width")->as_or<float>(0.0F);
        }
        if (props.contains("track_height")) {
            track_height_ = props.at("track_height")->as_or<float>(0.0F);
        }
        if (props.contains("thumb_inset")) {
            thumb_inset_ = props.at("thumb_inset")->as_or<float>(0.0F);
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("border_width")) {
            border_width_ = props.at("border_width")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
    }

  protected:
    // ---- 继承扩展点：分阶段绘制 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = track_width_, .height = track_height_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const bool on = value();
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color active = active_color_.value_or(inherit_theme(ctx).primary);
        Color inactive = inactive_color_;
        Color thumb = thumb_color_;
        if (!enabled_) {
            active = Color{176, 176, 180, 255};
            inactive = Color{210, 210, 214, 255};
            thumb = Color{240, 240, 242, 255};
        }
        Color track = on ? active : inactive;
        if (enabled_ && (hovered() || pressed_)) {
            track = track.shaded(pressed_ ? 0.80F : 0.90F);  // 悬停/按下反馈
        }
        paint_track(p, bounds, track, on);
        paint_thumb(p, bounds, thumb, on);
    }

    /// @brief 绘制圆角轨道（关闭态可选描边）。
    /// @param p 绘制器。
    /// @param bounds 轨道区域。
    /// @param track 轨道填充色（已含悬停/按下压暗）。
    /// @param on 开关状态（关闭态才画描边）。
    virtual auto paint_track(Painter &p, const Rect &bounds, Color track, bool on) -> void {
        const float radius = bounds.size.height * 0.5F;
        p.fill_rounded_rect(bounds, radius, track);
        if (!on && border_width_ > 0.0F && border_color_.has_value()) {
            const Color bc = enabled_ ? *border_color_ : border_color_->with_alpha(128);
            p.draw_rounded_border(bounds, radius, border_width_, bc);
        }
    }

    /// @brief 绘制圆形滑块（开=右端，关=左端）。
    /// @param p 绘制器。
    /// @param bounds 轨道区域（据此定位滑块端点）。
    /// @param thumb 滑块填充色。
    /// @param on 开关状态。
    virtual auto paint_thumb(Painter &p, const Rect &bounds, Color thumb, bool on) -> void {
        const float d = bounds.size.height - (2.0F * thumb_inset_);
        const float knob_x = on ? (bounds.right() - d - thumb_inset_) : (bounds.origin.x + thumb_inset_);
        const Rect knob{.origin = Point{.x = knob_x, .y = bounds.origin.y + thumb_inset_},
                        .size = Size{.width = d, .height = d}};
        p.fill_rounded_rect(knob, d * 0.5F, thumb);
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<bool> binding_;  // 声明须在 value_ 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<bool> value_;
    std::function<void(bool)> on_changed_;
    std::optional<Color> active_color_;  ///< 开启态轨道色；空 = 跟随主题 primary
    Color inactive_color_ = Color{180, 180, 180, 255};  ///< 关闭态轨道色
    Color thumb_color_ = colors::AURORA_WHITE;  ///< 滑块（圆形）颜色
    float track_width_ = 44.0F;  ///< 轨道宽 dp
    float track_height_ = 24.0F;  ///< 轨道高 dp
    float thumb_inset_ = 2.0F;  ///< 滑块与轨道边缘间距 dp
    std::optional<Color> border_color_;  ///< 关闭态轨道描边色；空 = 不描边
    float border_width_ = 0.0F;  ///< 关闭态描边宽 dp；0 = 不描边
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
