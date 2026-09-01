#pragma once

#include <algorithm>
#include <functional>
#include <optional>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/signal_view.h"
#include "aurora/theming/theme_scope.h" // inherit_theme：color 未显式设置时跟随主题 primary
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 进度指示器（叶控件）：线性进度条，值范围 [0,1]。
 *
 * 支持两种值来源（与 Checkbox 一致的响应式模式）：
 * - `Reactive<double>`：内部持有状态，变化触发重绘；
 * - `Binding<double>`：双向绑定到上游 `State<double>`，随其变化刷新。
 *
 * 视觉（对标 Material LinearProgressIndicator / Qt QProgressBar）：
 * - 圆角胶囊轨道 + 填充（`corner_radius < 0` 自动 = 厚度一半；0 = 直角）；
 * - `color` 未显式设置时跟随主题 `Theme::primary`（ThemeScope 换肤即生效）；
 * - 厚度可调（`set_thickness`，决定自然高度）。
 *
 * 继承扩展点（protected 虚函数）：`paint_track` / `paint_fill`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ProgressIndicator : public LeafWidget {
  public:
    ProgressIndicator() = default;
    explicit ProgressIndicator(Reactive<double> value) : m_value(std::move(value)) {}
    explicit ProgressIndicator(Binding<double> binding) : m_binding(std::move(binding)), m_value(m_binding.get()) {}

    [[nodiscard]] auto value() const -> double { return m_binding.bound() ? m_binding.get() : m_value.get(); }

    /// @brief 设置填充色（链式）。不调用则跟随主题 `Theme::primary`。
    auto set_color(Color c) -> ProgressIndicator & {
        m_color = c;
        return *this;
    }

    /// @brief 设置轨道底色（链式）。
    auto set_track_color(Color c) -> ProgressIndicator & {
        m_track_color = c;
        return *this;
    }

    /// @brief 设置厚度 dp（链式；决定控件自然高度）。
    auto set_thickness(float t) -> ProgressIndicator & {
        m_thickness = t > 0.0f ? t : 6.0f;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置圆角半径 dp（链式；< 0 自动 = 厚度一半；0 = 直角）。
    auto set_corner_radius(float r) -> ProgressIndicator & {
        m_corner_radius = r;
        mark_needs_paint();
        return *this;
    }

    auto set_value(double v) -> void {
        v = std::max(v, 0.0);
        v = std::min(v, 1.0);
        if (m_binding.bound()) {
            m_binding.set(v);
        }
        m_value = v;
        mark_needs_paint();
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        out.push_back(&m_value);
        if (m_binding.bound()) {
            out.push_back(m_binding.target());
        }
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ProgressIndicator"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ProgressIndicator",
            .properties = {
                { .name = "value", .type = "double", .default_value = "0.0", .required = false, .note = "进度值[0,1]", .json_type = "number", .enum_values = {}, .min_value = "0", .max_value = "1" },
                { .name = "color", .type = "Color", .default_value = "theme.primary", .required = false, .note = "填充色（缺省跟随主题 primary）", .json_type = "array" },
                { .name = "track_color", .type = "Color", .default_value = "{220,220,220,255}", .required = false, .note = "轨道底色", .json_type = "array" },
                { .name = "thickness", .type = "float", .default_value = "6.0", .required = false, .note = "厚度(dp)，决定自然高度", .json_type = "number", .enum_values = {}, .min_value = "0" },
                { .name = "corner_radius", .type = "float", .default_value = "-1.0", .required = false, .note = "圆角半径(dp)；<0 自动=厚度一半，0=直角", .json_type = "number" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = {},
            .children_policy = "none",
            .invariants = { "value >= 0 && value <= 1", "thickness > 0" },
            .examples = { "au::ProgressIndicator()" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["value"] = value();
        if (m_color.has_value()) {
            props["color"] = color_to_json(*m_color); // 未设置不输出：保留「跟随主题」语义
        }
        props["track_color"] = color_to_json(m_track_color);
        props["thickness"] = m_thickness;
        props["corner_radius"] = m_corner_radius;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("color")) {
            m_color = json_to_color(props["color"]);
        }
        if (props.contains("track_color")) {
            m_track_color = json_to_color(props["track_color"]);
        }
        if (props.contains("thickness")) {
            m_thickness = props["thickness"].get<float>();
        }
        if (props.contains("corner_radius")) {
            m_corner_radius = props["corner_radius"].get<float>();
        }
        if (props.contains("value")) {
            set_value(props["value"].get<double>());
        }
    }

  protected:
    // ---- 继承扩展点：分阶段绘制 ----

    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{ .width = c.max.width, .height = m_thickness });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const Color fill = m_color.value_or(inherit_theme(ctx).primary);
        const float radius = m_corner_radius >= 0.0f ? m_corner_radius : bounds.size.height * 0.5f;
        paint_track(p, bounds, m_track_color, radius);
        paint_fill(p, bounds, fill, radius);
    }

    /// @brief 绘制轨道底（圆角胶囊；radius=0 退化为直角）。
    virtual auto paint_track(Painter &p, const Rect &bounds, Color c, float radius) -> void {
        if (radius > 0.0f) {
            p.fill_rounded_rect(bounds, radius, c);
        } else {
            p.fill_rect(bounds, c);
        }
    }

    /// @brief 绘制进度填充（宽度按值比例）。
    virtual auto paint_fill(Painter &p, const Rect &bounds, Color c, float radius) -> void {
        const float w = bounds.size.width * static_cast<float>(value());
        if (w <= 0.0f) {
            return;
        }
        const Rect fill{ .origin = bounds.origin, .size = Size{ .width = w, .height = bounds.size.height } };
        if (radius > 0.0f) {
            p.fill_rounded_rect(fill, radius, c);
        } else {
            p.fill_rect(fill, c);
        }
    }

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    Binding<double> m_binding; // 声明须在 m_value 之前（同 checkbox.h 的初始化顺序修复）
    Reactive<double> m_value;
    std::optional<Color> m_color;                      ///< 填充色；空 = 跟随主题 primary
    Color m_track_color = Color{ 220, 220, 220, 255 }; ///< 轨道底色
    float m_thickness = 6.0f;                          ///< 厚度 dp（自然高度）
    float m_corner_radius = -1.0f;                     ///< 圆角半径 dp；< 0 自动 = 厚度一半
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

} // namespace aurora
