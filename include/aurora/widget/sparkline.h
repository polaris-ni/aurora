#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/chart_common.h"
#include "aurora/widget/widget.h"
#include "aurora/core/accessibility.h"

namespace aurora {

/// @brief Sparkline 属性（聚合；所有字段均有默认值）。
struct SparklineProps {
    std::vector<double> values;  ///< 单系列数据（等距，x = 索引）
    std::optional<Color> color;  ///< 线条色；空 = 按索引 0 取内置色板
    float line_width = 1.5F;  ///< 线宽（dp）
    bool show_end_dot = true;  ///< 末端数据点圆点
    float dot_radius = 2.0F;  ///< 末端圆点半径（dp）
    EdgeInsets padding{.left = 2.0F, .top = 2.0F, .right = 2.0F, .bottom = 2.0F};  ///< 图内留白
};

/**
 * @brief 迷你折线（叶控件，切片 4；契约见 specification/04-widget.md §3.8）。
 *
 * **无轴、无网格、无图例、无交互**——最薄的图表控件，用于表格 / 卡片内的趋势缩览。
 * 值域直接取数据 min/max（退化时回退 `[0,1]`），绘制 = `stroke_polyline` + 末端圆点。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Sparkline : public LeafWidget, public SparklineProps {
  public:
    Sparkline() = default;
    explicit Sparkline(SparklineProps props) : SparklineProps(std::move(props)) {}

    [[nodiscard]] static auto defaults() -> SparklineProps { return SparklineProps{}; }

    auto set_values(std::vector<double> v) -> Sparkline & {
        values = std::move(v);
        mark_needs_paint();
        grow_.replay();
        return *this;
    }
    auto set_color(Color c) -> Sparkline & {
        color = c;
        mark_needs_paint();
        return *this;
    }
    auto set_line_width(float w) -> Sparkline & {
        line_width = w;
        mark_needs_paint();
        return *this;
    }
    auto set_show_end_dot(bool v) -> Sparkline & {
        show_end_dot = v;
        mark_needs_paint();
        return *this;
    }
    auto set_padding(const EdgeInsets &p) -> Sparkline & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Sparkline"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&grow_.signal()); }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`（D8）—— 推断表不识 `Sparkline`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override {
        return AccessibilityRole::Image;
    }

    [[nodiscard]] auto accessibility_label() const -> std::string override;
    [[nodiscard]] auto accessibility_value() const -> std::string override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        Size s = c.constrain(Size{.width = c.max.width, .height = c.max.height});
        if (!std::isfinite(c.max.width)) {
            s.width = 80.0F;
        }
        if (!std::isfinite(c.max.height)) {
            s.height = 24.0F;
        }
        return Size{.width = std::max(s.width, 0.0F), .height = std::max(s.height, 0.0F)};
    }

    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const float left = bounds.origin.x + padding.left;
        const float top = bounds.origin.y + padding.top;
        const float w = std::max(0.0F, bounds.size.width - padding.left - padding.right);
        const float h = std::max(0.0F, bounds.size.height - padding.top - padding.bottom);
        if (w <= 0.0F || h <= 0.0F || values.size() < 2 || line_width <= 0.0F) {
            return;
        }
        double lo = 0.0;
        double hi = 0.0;
        bool have = false;
        for (const double v : values) {
            if (!std::isfinite(v)) {
                continue;
            }
            lo = have ? std::min(lo, v) : v;
            hi = have ? std::max(hi, v) : v;
            have = true;
        }
        if (!have || !(hi > lo)) {
            hi = lo + 1.0;
        }
        const Theme theme = inherit_theme(ctx);
        const Color c = color.value_or(chart_palette(0));
        std::vector<Point> pts;
        pts.reserve(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double v = std::isfinite(values[i]) ? values[i] : lo;
            const float t = static_cast<float>(i) / static_cast<float>(values.size() - 1U);
            const auto yv = static_cast<float>((hi - v) / (hi - lo));
            pts.push_back(Point{.x = left + (t * w), .y = top + (yv * h)});
        }
        // grow-in：只绘制到 t 对应的折线前缀（末段按分数插值端点）
        std::vector<Point> drawn = pts;
        if (const auto grow_t = static_cast<float>(grow_.progress()); grow_t < 1.0F) {
            const float span = static_cast<float>(pts.size() - 1) * grow_t;
            const auto whole = static_cast<std::size_t>(std::floor(span));
            const float frac = span - static_cast<float>(whole);
            const std::size_t keep = std::min(whole + 1U, pts.size());
            drawn.assign(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(keep));
            if (keep < pts.size() && frac > 0.0F) {
                const Point &a = pts[keep - 1U];
                const Point &b = pts[keep];
                drawn.push_back(Point{.x = a.x + ((b.x - a.x) * frac), .y = a.y + ((b.y - a.y) * frac)});
            }
        }
        p.stroke_polyline(drawn, line_width, c);
        if (show_end_dot && dot_radius > 0.0F && grow_.progress() >= 1.0) {
            const Point &last = pts.back();
            const float d = dot_radius * 2.0F;
            // 末端圆点须夹在控件内（D12：越界像素不会被脏区擦除）
            const float cx =
                std::clamp(last.x, bounds.origin.x + dot_radius, bounds.origin.x + bounds.size.width - dot_radius);
            const float cy =
                std::clamp(last.y, bounds.origin.y + dot_radius, bounds.origin.y + bounds.size.height - dot_radius);
            p.fill_rounded_rect(Rect{.origin = Point{.x = cx - dot_radius, .y = cy - dot_radius},
                                     .size = Size{.width = d, .height = d}},
                                dot_radius, c);
        }
        (void)theme;
    }

  private:
    ChartGrowIn grow_;
};

inline auto Sparkline::accessibility_label() const -> std::string {
    return "Sparkline, " + std::to_string(values.size()) + " points";
}

inline auto Sparkline::accessibility_value() const -> std::string {
    if (values.empty()) {
        return std::string{};
    }
    return std::to_string(values.back());
}

inline auto Sparkline::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Sparkline",
        .properties =
            {
                {.name = "values",
                 .type = "vector<double>",
                 .default_value = "[]",
                 .required = false,
                 .note = "单系列数据（等距）",
                 .json_type = "array"},
                {.name = "color",
                 .type = "Color",
                 .default_value = "palette[0]",
                 .required = false,
                 .note = "线条色；缺省取内置色板第 0 色",
                 .json_type = "array"},
                {.name = "line_width",
                 .type = "float",
                 .default_value = "1.5",
                 .required = false,
                 .note = "线宽(dp)",
                 .json_type = "number",
                 .min_value = "0"},
                {.name = "show_end_dot",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "末端数据点圆点",
                 .json_type = "boolean"},
                {.name = "dot_radius",
                 .type = "float",
                 .default_value = "2.0",
                 .required = false,
                 .note = "末端圆点半径(dp)",
                 .json_type = "number",
                 .min_value = "0"},
                {.name = "padding",
                 .type = "EdgeInsets",
                 .default_value = "{2,2,2,2}",
                 .required = false,
                 .note = "图内留白(dp)",
                 .json_type = "object"},
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
        .invariants = {"line_width >= 0"},
        .examples = {"au::Sparkline(au::SparklineProps{ .values = {1, 3, 2, 5, 4} })"},
    };
}

inline auto Sparkline::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["values"] = double_vector_to_json(values);
    if (color.has_value()) {
        props["color"] = color_to_json(*color);
    }
    props["line_width"] = line_width;
    props["show_end_dot"] = show_end_dot;
    props["dot_radius"] = dot_radius;
    props["padding"] = edge_insets_to_json(padding);
}

inline auto Sparkline::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("values")) {
        values = json_to_double_vector(props["values"]);
    }
    if (props.contains("color") && props["color"].is_array()) {
        color = json_to_color(props["color"]);
    }
    if (props.contains("line_width") && props["line_width"].is_number()) {
        line_width = std::max(0.0F, props["line_width"].get<float>());
    }
    if (props.contains("show_end_dot") && props["show_end_dot"].is_boolean()) {
        show_end_dot = props["show_end_dot"].get<bool>();
    }
    if (props.contains("dot_radius") && props["dot_radius"].is_number()) {
        dot_radius = std::max(0.0F, props["dot_radius"].get<float>());
    }
    if (props.contains("padding")) {
        padding = json_to_edge_insets(props["padding"]);
    }
    mark_needs_paint();
}

}  // namespace aurora
