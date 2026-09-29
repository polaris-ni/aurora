#pragma once

#include <algorithm>
#include <cmath>
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

/// @brief 滑块（叶控件）：连续值 `double`（默认 [0,1]），拖拽设置。
///
/// 值来源与 `Checkbox` 一致（`Reactive<double>` / `Binding<double>` + `onChanged`）。
/// 指针按下并拖动时，按局部 x 位置映射到 [min,max]；松手结束。绘制为圆角轨道 + 圆形滑块。
///
/// 视觉（对标 Material 3 Slider / Qt QSlider）：
/// - `active_color` 未显式设置时跟随主题 `Theme::primary`（ThemeScope 换肤即生效）；
/// - 悬停/拖动时滑块调暗反馈；禁用（`set_enabled(false)`）灰化并忽略拖拽；
/// - `step > 0` 时值吸附到步进网格（对标 Qt `singleStep` / Flutter `divisions`）。
///
/// 继承扩展点（protected 虚函数）：`paint_track` / `paint_active_track` / `paint_thumb`，
/// 几何由 `track_rect` / `value_fraction` 提供，子类可单独覆盖某个绘制阶段。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Slider : public LeafWidget {
  public:
    Slider() = default;
    /// @brief 以响应式值源构造滑块。
    /// @param value 当前值的响应式信号（驱动绘制与回读）。
    /// @param on_changed 值变化回调（设值后经 clamp/步进吸附以终值触发）；空 = 不回调。
    explicit Slider(Reactive<double> value, std::function<void(double)> on_changed = {})
        : value_(std::move(value)), on_changed_(std::move(on_changed)) {}
    /// @brief 以双向绑定构造滑块（初值取 binding.get()）。
    /// @param binding 当前值的双向绑定（设值时同步写回绑定目标）。
    /// @param on_changed 值变化回调（设值后经 clamp/步进吸附以终值触发）；空 = 不回调。
    explicit Slider(Binding<double> binding, std::function<void(double)> on_changed = {})
        : binding_(std::move(binding)), value_(binding_.get()), on_changed_(std::move(on_changed)) {}

    /// @brief 设置取值区间（链式）。
    /// @param min 取值下界。
    /// @param max 取值上界（指针位置按 [min,max] 线性映射/反映射）。
    /// @return 自身引用，便于链式调用。
    auto set_range(double min, double max) -> Slider & {
        min_ = min;
        max_ = max;
        return *this;
    }
    /// @brief 设置值变化回调（链式）。
    /// @param cb 新值回调（设值后经 clamp/步进吸附以终值触发）；空 = 不回调。
    /// @return 自身引用，便于链式调用。
    auto set_on_changed(std::function<void(double)> cb) -> Slider & {
        on_changed_ = std::move(cb);
        return *this;
    }

    /// @brief 设置已填充轨道与滑块颜色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c 激活态颜色。
    /// @return 自身引用，便于链式调用。
    auto set_active_color(Color c) -> Slider & {
        active_color_ = c;
        return *this;
    }

    /// @brief 设置未填充轨道颜色（链式）。
    /// @param c 未填充（背景）轨道颜色。
    /// @return 自身引用，便于链式调用。
    auto set_inactive_color(Color c) -> Slider & {
        inactive_color_ = c;
        return *this;
    }

    /// @brief 设置滑块颜色（链式）。不调用则与激活色一致。
    /// @param c 滑块（拇指）颜色。
    /// @return 自身引用，便于链式调用。
    auto set_thumb_color(Color c) -> Slider & {
        thumb_color_ = c;
        return *this;
    }

    /// @brief 设置轨道高度 dp（链式）。
    /// @param h 轨道高度（dp）；<= 0 时回落为默认 6。
    /// @return 自身引用，便于链式调用。
    auto set_track_height(float h) -> Slider & {
        track_height_ = h > 0.0F ? h : 6.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置滑块直径 dp（链式；< 0 表示自动 = 控件高 − 6）。
    /// @param d 滑块直径（dp）；负值为自动模式。
    /// @return 自身引用，便于链式调用。
    auto set_thumb_size(float d) -> Slider & {
        thumb_size_ = d;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置步进（链式；0 = 连续无级）。>0 时值吸附到 min + k×step 网格。
    /// @param s 步进步长；<= 0 归一为 0（连续）。
    /// @return 自身引用，便于链式调用。
    auto set_step(double s) -> Slider & {
        step_ = s > 0.0 ? s : 0.0;
        return *this;
    }
    /// @brief 读取当前步进。
    /// @return 步进步长；0 = 连续无级。
    [[nodiscard]] auto step() const -> double { return step_; }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略拖拽。
    /// @param v true = 可交互；false = 禁用（灰化、吞指针事件、不改值）。
    /// @return 自身引用，便于链式调用。
    auto set_enabled(bool v) -> Slider & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    /// @brief 读取启用状态。
    /// @return true = 可交互。
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 读取当前值。
    /// @return 绑定态取 Binding::get()，否则取内部响应式信号；恒在 [min,max] 内且已按 step 吸附。
    [[nodiscard]] auto value() const -> double { return binding_.bound() ? binding_.get() : value_.get(); }

    /// @brief 设置当前值：clamp 到 [min,max]，step > 0 时吸附到 min + k×step 网格。
    /// @param v 目标值（clamp/吸附前的原始值）。
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
        notify_accessibility_event(AccessibilityEvent{.kind = AccessibilityEventKind::ValueChanged, .target = this});
    }

    /// @brief 收集本控件订阅的信号：内部值信号；绑定态再追加 Binding 的目标信号。
    /// @param out 出参：信号视图追加到该向量尾部。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&value_);
        if (binding_.bound()) {
            out.push_back(binding_.target());
        }
    }

    /// @brief 控件类型名。
    /// @return 静态字符串 "Slider"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Slider"; }

    /// @brief 无障碍值：当前取值的十进制串（`std::to_string` 的 6 位小数格式）。
    /// @return 当前 value() 的字符串形式。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override { return std::to_string(value()); }

    /// @brief 无障碍取值域：min/max/step 取控件既有刻度，value 取当前值。
    /// @return 由既有刻度与当前值组装的 AccessibilityRange。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_range() const -> std::optional<AccessibilityRange> override {
        return AccessibilityRange{.min = min_, .max = max_, .step = step_, .value = value()};
    }

    /// @brief 读屏 Value 动作：设值（走 `set_value` 既有 clamp + step 语义）。
    ///
    /// 与真实拖动的差异：无 drag 手势，但 `on_changed` 一致触发（读屏操作本就是语义直通）。
    /// @param req 动作请求；action 为 AccessibilityAction::Value 时以 req.number 设值。
    /// @return Value 动作恒为 true（已处理）；其余动作回退基类结果。
    /// @note Side-effects: mutates state
    auto perform_accessibility_action(const AccessibilityActionRequest &req) -> bool override {
        if (req.action == AccessibilityAction::Value) {
            set_value(req.number);
            return true;
        }
        return Widget::perform_accessibility_action(req);
    }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Slider 的控件描述符：属性键与默认值、on_changed 事件、无子节点、取值不变量与示例。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Slider",
            .properties =
                {
                    {.name = "value",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Current value",
                     .json_type = "number"},
                    {.name = "min",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Min value",
                     .json_type = "number"},
                    {.name = "max",
                     .type = "double",
                     .default_value = "1.0",
                     .required = false,
                     .note = "Max value",
                     .json_type = "number"},
                    {.name = "step",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Step; 0 = continuous",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "active_color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "Filled track and thumb color (defaults to theme primary)",
                     .json_type = "array"},
                    {.name = "inactive_color",
                     .type = "Color",
                     .default_value = "{210,210,210,255}",
                     .required = false,
                     .note = "Unfilled track color",
                     .json_type = "array"},
                    {.name = "thumb_color",
                     .type = "Color",
                     .default_value = "active_color",
                     .required = false,
                     .note = "Thumb color (defaults to the active color)",
                     .json_type = "array"},
                    {.name = "track_height",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "Track height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "thumb_size",
                     .type = "float",
                     .default_value = "-1.0",
                     .required = false,
                     .note = "Thumb diameter (dp); <0 = widget height - 6",
                     .json_type = "number"},
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
            .invariants = {"min <= max", "value >= min", "value <= max"},
            .examples = {"au::Slider().set_range(0, 100)"},
        };
    }
    /// @brief 实例自描述入口。
    /// @return 与 describe_static() 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 指针事件处理：Press 开始拖拽并按局部 x 设值，按住 Move 持续设值，Release 结束拖拽。
    /// @param e 鼠标事件；禁用态吞掉事件（置 is_handled、不改值），启用态消费 Press/Move/Release。
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
    /// @param entered true = 悬停进入，false = 离开；启用态下标记重绘以刷新明暗。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled_) {
            mark_needs_paint();
        }
    }

    /// @brief 序列化当前属性：值/区间/刻度/颜色/几何/启用态（step 为 0、颜色未显式设置时不输出）。
    /// @param props 出参：键值写入该 JSON 对象（先由基类写入通用属性）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("value", value());
        props.set("min", min_);
        props.set("max", max_);
        if (step_ > 0.0) {
            props.set("step", step_);
        }
        if (active_color_.has_value()) {
            props.set("active_color", color_to_json(*active_color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("inactive_color", color_to_json(inactive_color_));
        if (thumb_color_.has_value()) {
            props.set("thumb_color", color_to_json(*thumb_color_));
        }
        props.set("track_height", track_height_);
        props.set("thumb_size", thumb_size_);
        props.set("enabled", Json{enabled_});
    }

    /// @brief 从 JSON 恢复属性：仅读取存在的键，缺失键保持当前值；value 经 set_value 写入（走 clamp/吸附）。
    /// @param props 来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("min")) {
            min_ = props.at("min")->as_or<double>(0.0);
        }
        if (props.contains("max")) {
            max_ = props.at("max")->as_or<double>(0.0);
        }
        if (props.contains("step")) {
            step_ = props.at("step")->as_or<double>(0.0);
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
        if (props.contains("track_height")) {
            track_height_ = props.at("track_height")->as_or<float>(0.0F);
        }
        if (props.contains("thumb_size")) {
            thumb_size_ = props.at("thumb_size")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
        if (props.contains("value")) {
            set_value(props.at("value")->as_or<double>(0.0));
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
    /// @param bounds 本控件布局矩形。
    /// @return 内缩后的轨道矩形（高为 track_height_，垂直居中于 bounds）。
    [[nodiscard]] auto track_rect(const Rect &bounds) const -> Rect {
        constexpr float inset = 4.0F;
        return Rect{.origin = Point{.x = bounds.origin.x + inset,
                                    .y = bounds.origin.y + ((bounds.size.height - track_height_) * 0.5F)},
                    .size = Size{.width = bounds.size.width - (2.0F * inset), .height = track_height_}};
    }

    /// @brief 当前值在 [min,max] 中的归一化占比 [0,1]。
    /// @return 归一化占比；max_ <= min_（区间退化）时为 0。
    [[nodiscard]] auto value_fraction() const -> float {
        return (max_ > min_) ? static_cast<float>((value() - min_) / (max_ - min_)) : 0.0F;
    }

    /// @brief 绘制未填充轨道（圆角胶囊）。
    /// @param p 绘制器。
    /// @param track 轨道矩形（track_rect 的结果）。
    /// @param c 轨道底色。
    virtual auto paint_track(Painter &p, const Rect &track, Color c) -> void {
        p.fill_rounded_rect(track, track.size.height * 0.5F, c);
    }

    /// @brief 绘制已填充轨道（从左端到当前值）。
    /// @param p 绘制器。
    /// @param track 轨道矩形；按其宽乘 value_fraction 得填充宽。
    /// @param c 填充色。
    virtual auto paint_active_track(Painter &p, const Rect &track, Color c) -> void {
        const float w = value_fraction() * track.size.width;
        if (w <= 0.0F) {
            return;
        }
        p.fill_rounded_rect(Rect{.origin = track.origin, .size = Size{.width = w, .height = track.size.height}},
                            track.size.height * 0.5F, c);
    }

    /// @brief 绘制圆形滑块（中心在当前值位置）。
    /// @param p 绘制器。
    /// @param bounds 本控件布局矩形（thumb_size_ 未设时按其中高减 6dp 推直径）。
    /// @param track 轨道矩形（水平定位基准）。
    /// @param c 滑块颜色。
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
    Binding<double> binding_;  // 声明须在 value_ 之前（同 checkbox.h 的初始化顺序修复）
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
