#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>

#include "aurora/core/a11y_types.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 进度指示器（叶控件）：线性进度条，值范围 [0,1]。
///
/// 支持两种值来源（与 Checkbox 一致的响应式模式）：
/// - `Reactive<double>`：内部持有状态，变化触发重绘；
/// - `Binding<double>`：双向绑定到上游 `State<double>`，随其变化刷新。
///
/// 视觉（对标 Material LinearProgressIndicator / Qt QProgressBar）：
/// - 圆角胶囊轨道 + 填充（`corner_radius < 0` 自动 = 厚度一半；0 = 直角）；
/// - `color` 未显式设置时跟随主题 `Theme::primary`（ThemeScope 换肤即生效）；
/// - 厚度可调（`set_thickness`，决定自然高度）。
///
/// 继承扩展点（protected 虚函数）：`paint_track` / `paint_fill`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class ProgressIndicator : public LeafWidget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 挂载了输入语义时为 true，纯展示时为 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    ProgressIndicator() = default;
    /// @brief 以响应式进度值构造。
    /// @param value 进度值（[0,1]），与持有方共享同一底层状态。
    explicit ProgressIndicator(Reactive<double> value) : value_(std::move(value)) {}
    /// @brief 以上游绑定构造：每次读取走绑定的 `State<double>`。
    /// @param binding 进度状态绑定，内部初值取其当前读数。
    explicit ProgressIndicator(Binding<double> binding) : binding_(std::move(binding)), value_(binding_.get()) {}

    /// @brief 当前进度值：绑定态读取上游 `State<double>`，未绑定读取内部 `Reactive<double>`。
    /// @return 进度值（[0,1]）。
    [[nodiscard]] auto value() const -> double { return binding_.bound() ? binding_.get() : value_.get(); }

    /// @brief 设置填充色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c [in] 填充色。
    /// @return 引用自身，便于链式调用。
    auto set_color(Color c) -> ProgressIndicator & {
        color_ = c;
        return *this;
    }

    /// @brief 设置轨道底色（链式）。
    /// @param c [in] 轨道底色。
    /// @return 引用自身，便于链式调用。
    auto set_track_color(Color c) -> ProgressIndicator & {
        track_color_ = c;
        return *this;
    }

    /// @brief 设置厚度 dp（链式；决定控件自然高度）。
    /// @param t [in] 厚度 dp；非正值回退默认 6.0。设置后标记重排。
    /// @return 引用自身，便于链式调用。
    auto set_thickness(float t) -> ProgressIndicator & {
        thickness_ = t > 0.0F ? t : 6.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；< 0 自动 = 厚度一半；0 = 直角）。
    /// @param r [in] 圆角半径 dp。设置后标记重绘。
    /// @return 引用自身，便于链式调用。
    auto set_corner_radius(float r) -> ProgressIndicator & {
        corner_radius_ = r;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置进度值：夹取到 [0,1]；绑定态同步写入上游 `State`，并标记重绘。
    /// @param v [in] 新进度值。
    auto set_value(double v) -> void {
        v = std::max(v, 0.0);
        v = std::min(v, 1.0);
        if (binding_.bound()) {
            binding_.set(v);
        }
        value_ = v;
        mark_needs_paint();
    }

    /// @brief 收集重建依赖的信号：内部 `value_` 与绑定态的上游 `State` 目标。
    /// @param out [out] 追加收集到的信号视图。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&value_);
        if (binding_.bound()) {
            out.push_back(binding_.target());
        }
    }

    /// @brief 控件类型名。
    /// @return 静态字符串 "ProgressIndicator"。
    [[nodiscard]] auto type_name() const -> const char * override { return "ProgressIndicator"; }

    /// @brief 无障碍值：当前进度的十进制串（值域 [0,1]，`std::to_string` 格式）。
    /// @return `std::to_string(value())` 的结果（小数点后 6 位定长）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_value() const -> std::string override { return std::to_string(value()); }

    /// @brief 无障碍取值域：进度恒为 [0,1] 只读区间（`step` 0 = 连续）。
    /// @return 含当前值的 AccessibilityRange（min 0、max 1、step 0）。
    /// @note Side-effects: reads state
    [[nodiscard]] auto accessibility_range() const -> std::optional<AccessibilityRange> override {
        return AccessibilityRange{.min = 0.0, .max = 1.0, .step = 0.0, .value = value()};
    }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 属性键（value / color / track_color / thickness / corner_radius 等）、
    ///         不变量与示例组成的静态描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ProgressIndicator",
            .properties =
                {
                    {.name = "value",
                     .type = "double",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Progress value [0,1]",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0",
                     .max_value = "1"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "theme.primary",
                     .required = false,
                     .note = "Fill color (defaults to theme primary)",
                     .json_type = "array"},
                    {.name = "track_color",
                     .type = "Color",
                     .default_value = "{220,220,220,255}",
                     .required = false,
                     .note = "Track color",
                     .json_type = "array"},
                    {.name = "thickness",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "Thickness (dp), determines the natural height",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "corner_radius",
                     .type = "float",
                     .default_value = "-1.0",
                     .required = false,
                     .note = "Corner radius (dp); <0 = half the thickness, 0 = square corners",
                     .json_type = "number"},
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
            .events = {},
            .children_policy = "none",
            .invariants = {"value >= 0 && value <= 1", "thickness > 0"},
            .examples = {"au::ProgressIndicator()"},
        };
    }
    /// @brief 实例自描述：与 `describe_static` 一致（无实例级差异）。
    /// @return 本控件的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化进度专属属性；`color` 未显式设置时不输出，保留「跟随主题」语义。
    /// @param props [out] 写入的属性 JSON 对象（先叠加基类属性）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("value", value());
        if (color_.has_value()) {
            props.set("color", color_to_json(*color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("track_color", color_to_json(track_color_));
        props.set("thickness", thickness_);
        props.set("corner_radius", corner_radius_);
    }

    /// @brief 从 JSON 恢复属性：色值 / 厚度 / 圆角直接覆写，`value` 经 `set_value` 夹取并触发重绘。
    /// @param props [in] 属性 JSON 对象；缺失的键保持当前值不变。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("color")) {
            color_ = json_to_color(*props.at("color"));
        }
        if (props.contains("track_color")) {
            track_color_ = json_to_color(*props.at("track_color"));
        }
        if (props.contains("thickness")) {
            thickness_ = props.at("thickness")->as_or<float>(0.0F);
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props.at("corner_radius")->as_or<float>(0.0F);
        }
        if (props.contains("value")) {
            set_value(props.at("value")->as_or<double>(0.0));
        }
    }

  protected:
    // ---- 继承扩展点：分阶段绘制 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = c.max.width, .height = thickness_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Color fill = color_.value_or(inherit_theme(ctx).primary);
        const float radius = corner_radius_ >= 0.0F ? corner_radius_ : bounds.size.height * 0.5F;
        paint_track(p, bounds, track_color_, radius);
        paint_fill(p, bounds, fill, radius);
    }

    /// @brief 绘制轨道底（圆角胶囊；radius=0 退化为直角）。
    /// @param p 绘制器。
    /// @param bounds 轨道区域。
    /// @param c 轨道底色。
    /// @param radius 圆角半径。
    virtual auto paint_track(Painter &p, const Rect &bounds, Color c, float radius) -> void {
        if (radius > 0.0F) {
            p.fill_rounded_rect(bounds, radius, c);
        } else {
            p.fill_rect(bounds, c);
        }
    }

    /// @brief 绘制进度填充（宽度按值比例）。
    /// @param p 绘制器。
    /// @param bounds 轨道区域（填充取其左侧按 value() 比例的一段）。
    /// @param c 填充色。
    /// @param radius 圆角半径。
    virtual auto paint_fill(Painter &p, const Rect &bounds, Color c, float radius) -> void {
        const float w = bounds.size.width * static_cast<float>(value());
        if (w <= 0.0F) {
            return;
        }
        const Rect fill{.origin = bounds.origin, .size = Size{.width = w, .height = bounds.size.height}};
        if (radius > 0.0F) {
            p.fill_rounded_rect(fill, radius, c);
        } else {
            p.fill_rect(fill, c);
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<double> binding_;  // 声明须在 value_ 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<double> value_;
    std::optional<Color> color_;  ///< 填充色；空 = 跟随主题 primary
    Color track_color_ = Color{220, 220, 220, 255};  ///< 轨道底色
    float thickness_ = 6.0F;  ///< 厚度 dp（自然高度）
    float corner_radius_ = -1.0F;  ///< 圆角半径 dp；< 0 自动 = 厚度一半
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

}  // namespace aurora
