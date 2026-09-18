#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/i18n/format.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/chart_common.h"
#include "aurora/widget/widget.h"
#include "aurora/core/accessibility.h"

namespace aurora {

/// @brief ScatterChart 属性（聚合；所有字段均有默认值）。
struct ScatterChartProps {
    std::vector<ScatterSeries> series;  ///< 散点系列（显式 `ChartPoint` 坐标）
    bool show_crosshair = true;         ///< 悬停时是否绘制十字准线（吸附最近点 x）
    ChartAxisSpec axis_x;               ///< x 轴（Linear）
    ChartAxisSpec axis_y;               ///< y 轴（Linear）
    ChartLegendSpec legend;
    EdgeInsets padding{8.0F, 8.0F, 8.0F, 8.0F};
};

/**
 * @brief 散点图控件（叶控件，切片 6；契约见 specification/04-widget.md §3.8）。
 *
 * x / y 双 `LinearScale`（域由数据推导，可经 `axis_*.min/max` 覆盖），圆点绘制。
 * 命中按**最近点欧氏距离**（阈值 = `dot_radius + 4dp`），与渲染同源于同一组比例尺（D6）。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ScatterChart : public LeafWidget, public ScatterChartProps {
  public:
    ScatterChart() = default;
    explicit ScatterChart(ScatterChartProps props) : ScatterChartProps(std::move(props)) {}

    [[nodiscard]] static auto defaults() -> ScatterChartProps { return ScatterChartProps{}; }

    /// @brief 点击（抬起）命中数据点时触发：`(系列索引, 点索引)`。旁挂，不进序列化面。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    auto set_series(std::vector<ScatterSeries> s) -> ScatterChart & {
        series = std::move(s);
        mark_needs_layout();
        mark_needs_paint();
        grow_.replay();
        return *this;
    }
    auto set_axis_x(ChartAxisSpec a) -> ScatterChart & {
        axis_x = a;
        mark_needs_layout();
        return *this;
    }
    auto set_axis_y(ChartAxisSpec a) -> ScatterChart & {
        axis_y = a;
        mark_needs_layout();
        return *this;
    }
    auto set_legend(ChartLegendSpec l) -> ScatterChart & {
        legend = l;
        mark_needs_layout();
        return *this;
    }
    auto set_padding(const EdgeInsets &p) -> ScatterChart & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ScatterChart"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&grow_.signal()); }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto on_hover_change(bool entered) -> void override {
        hover_ = entered;
        if (!entered && (hovered_point_.has_value() || legend_hover_.has_value())) {
            hovered_point_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`（D8）—— 推断表不识 `ScatterChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override {
        return AccessibilityRole::Image;
    }

    [[nodiscard]] auto accessibility_label() const -> std::string override;
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 点索引）；无悬停为空。
    [[nodiscard]] auto hovered_point() const -> std::optional<std::pair<int, int>> { return hovered_point_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

  private:
    struct Geometry {
        Rect plot{};
        LinearScale x_scale{};
        LinearScale y_scale{};
        std::vector<Rect> legend_rects;
    };

    [[nodiscard]] auto domain_of(bool x_axis) const -> std::pair<double, double>;
    [[nodiscard]] auto scale_of(bool x_axis) const -> LinearScale;
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    [[nodiscard]] auto legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t>;
    [[nodiscard]] auto nearest_point(const Geometry &g, const Point &local) const
        -> std::optional<std::pair<int, int>>;

    Geometry geom_;
    ChartGrowIn grow_;
    std::optional<std::pair<int, int>> hovered_point_;
    std::optional<std::size_t> legend_hover_;
};

inline auto ScatterChart::domain_of(bool x_axis) const -> std::pair<double, double> {
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    for (const ScatterSeries &s : series) {
        for (const ChartPoint &pt : s.points) {
            const double v = x_axis ? pt.x : pt.y;
            if (!std::isfinite(v)) {
                continue;
            }
            lo = have ? std::min(lo, v) : v;
            hi = have ? std::max(hi, v) : v;
            have = true;
        }
    }
    if (!have) {
        lo = 0.0;
        hi = 1.0;
    }
    const ChartAxisSpec &spec = x_axis ? axis_x : axis_y;
    if (spec.min.has_value()) {
        lo = *spec.min;
    }
    if (spec.max.has_value()) {
        hi = *spec.max;
    }
    return {lo, hi};
}

inline auto ScatterChart::scale_of(bool x_axis) const -> LinearScale {
    const auto [lo, hi] = domain_of(x_axis);
    const ChartAxisSpec &spec = x_axis ? axis_x : axis_y;
    if (spec.min.has_value() && spec.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, spec.tick_count);
    }
    return LinearScale::from_domain(lo, hi, spec.tick_count);
}

inline auto ScatterChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    g.x_scale = scale_of(true);
    g.y_scale = scale_of(false);

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = padding.left;
    float top = padding.top;
    float right = std::max(padding.left, size.width - padding.right);
    float bottom = std::max(padding.top, size.height - padding.bottom);

    if (legend.visible && !series.empty()) {
        if (legend.position == LegendPosition::Top) {
            top += line_h;
        } else if (legend.position == LegendPosition::Bottom) {
            bottom -= line_h;
        } else {
            float widest = 0.0F;
            for (const ScatterSeries &s : series) {
                widest = std::max(widest, render::FontEngine::measure_width(s.name, font));
            }
            right = std::max(left, right - (widest + 22.0F));
        }
    }
    if (axis_x.visible) {
        bottom -= line_h;
        if (!axis_x.label.empty()) {
            bottom -= line_h;
        }
    }
    if (axis_y.visible) {
        float widest = 0.0F;
        const int digits = tick_digits(g.y_scale.step());
        for (const double t : g.y_scale.ticks()) {
            widest = std::max(widest, render::FontEngine::measure_width(format_number(t, Locale{}, digits), font));
        }
        left += widest + 6.0F;
        if (!axis_y.label.empty()) {
            left += line_h;
        }
    }
    g.plot = Rect{.origin = Point{.x = left, .y = top},
                  .size = Size{.width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)}};

    if (legend.visible && !series.empty()) {
        float cursor_x = g.plot.origin.x;
        float cursor_y = g.plot.origin.y;
        if (legend.position == LegendPosition::Top) {
            cursor_y = padding.top;
        } else if (legend.position == LegendPosition::Bottom) {
            cursor_y = size.height - padding.bottom - line_h;
        } else {
            cursor_x = g.plot.right() + 8.0F;
            cursor_y = g.plot.origin.y;
        }
        for (const ScatterSeries &s : series) {
            const float name_w = render::FontEngine::measure_width(s.name, font);
            const float item_w = 14.0F + name_w;
            if (legend.position != LegendPosition::Right && cursor_x + item_w > size.width - padding.right) {
                break;
            }
            g.legend_rects.push_back(
                Rect{.origin = Point{.x = cursor_x, .y = cursor_y}, .size = Size{.width = item_w, .height = line_h}});
            if (legend.position == LegendPosition::Right) {
                cursor_y += line_h;
            } else {
                cursor_x += item_w + 12.0F;
            }
        }
    }
    return g;
}

inline auto ScatterChart::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size s = c.constrain(Size{.width = c.max.width, .height = c.max.height});
    if (!std::isfinite(c.max.width)) {
        s.width = 300.0F;
    }
    if (!std::isfinite(c.max.height)) {
        s.height = 200.0F;
    }
    s.width = std::max(s.width, 0.0F);
    s.height = std::max(s.height, 0.0F);
    geom_ = compute_geometry(s, inherit_theme(ctx).font);
    return s;
}

inline auto ScatterChart::legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

inline auto ScatterChart::nearest_point(const Geometry &g, const Point &local) const
    -> std::optional<std::pair<int, int>> {
    if (g.plot.size.width <= 0.0F || g.plot.size.height <= 0.0F || series.empty()) {
        return std::nullopt;
    }
    float best = std::numeric_limits<float>::max();
    std::optional<std::pair<int, int>> best_hit;
    for (std::size_t k = 0; k < series.size(); ++k) {
        const float threshold = series[k].dot_radius + 4.0F;
        for (std::size_t i = 0; i < series[k].points.size(); ++i) {
            const ChartPoint &pt = series[k].points[i];
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
                continue;
            }
            const float px = g.x_scale.to_px(pt.x, g.plot.origin.x, g.plot.right());
            const float py = g.y_scale.to_px(pt.y, g.plot.bottom(), g.plot.origin.y);
            const float dx = local.x - px;
            const float dy = local.y - py;
            const float d = std::sqrt((dx * dx) + (dy * dy));
            if (d <= threshold && d < best) {
                best = d;
                best_hit = std::pair<int, int>{static_cast<int>(k), static_cast<int>(i)};
            }
        }
    }
    return best_hit;
}

inline auto ScatterChart::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Move) {
        const auto legend = legend_hit(geom_, e.local_position);
        const auto point = legend.has_value() ? std::nullopt : nearest_point(geom_, e.local_position);
        if (legend != legend_hover_ || point != hovered_point_) {
            legend_hover_ = legend;
            hovered_point_ = point;
            mark_needs_paint();
        }
        e.is_handled = true;
        return;
    }
    if (e.action == MouseAction::Release) {
        if (const auto point = nearest_point(geom_, e.local_position); point.has_value() && on_point_tapped) {
            on_point_tapped(point->first, point->second);
        }
        e.is_handled = true;
        return;
    }
    Widget::on_pointer_event(e);
}

inline auto ScatterChart::accessibility_label() const -> std::string {
    std::size_t total = 0;
    for (const ScatterSeries &s : series) {
        total += s.points.size();
    }
    return "ScatterChart, " + std::to_string(series.size()) + " series, " + std::to_string(total) + " points";
}

inline auto ScatterChart::accessibility_value() const -> std::string {
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const auto [k, i] = *hovered_point_;
    if (k < 0 || i < 0 || static_cast<std::size_t>(k) >= series.size() ||
        static_cast<std::size_t>(i) >= series[static_cast<std::size_t>(k)].points.size()) {
        return std::string{};
    }
    const ChartPoint &pt = series[static_cast<std::size_t>(k)].points[static_cast<std::size_t>(i)];
    return series[static_cast<std::size_t>(k)].name + ": (" + std::to_string(pt.x) + ", " + std::to_string(pt.y) + ")";
}

inline auto ScatterChart::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "ScatterChart",
        .properties =
            {
                {.name = "series",
                 .type = "vector<ScatterSeries>",
                 .default_value = "[]",
                 .required = false,
                 .note = "散点系列数组：[{name, points:[[x,y],...], color?, dot_radius}]",
                 .json_type = "array"},
                {.name = "show_crosshair",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "悬停时绘制吸附最近点的十字准线",
                 .json_type = "boolean"},
                {.name = "axis_x",
                 .type = "Json",
                 .default_value = "{}",
                 .required = false,
                 .note = "x 轴规格：{visible,label,tick_count,min,max,show_grid_lines}",
                 .json_type = "object"},
                {.name = "axis_y",
                 .type = "Json",
                 .default_value = "{}",
                 .required = false,
                 .note = "y 轴规格：{visible,label,tick_count,min,max,show_grid_lines}",
                 .json_type = "object"},
                {.name = "legend",
                 .type = "Json",
                 .default_value = "{\"visible\":true,\"position\":\"Top\"}",
                 .required = false,
                 .note = "图例规格：{visible,position:Top|Bottom|Right}",
                 .json_type = "object"},
                {.name = "padding",
                 .type = "EdgeInsets",
                 .default_value = "{8,8,8,8}",
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
        .events = {"on_point_tapped"},
        .children_policy = "none",
        .invariants = {"dot_radius >= 0"},
        .examples = {"au::ScatterChart(au::ScatterChartProps{ .series = {{ .name = \"A\", .points = "
                     "{{1, 2}, {3, 4}} }} })"},
    };
}

inline auto ScatterChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["series"] = scatter_series_vector_to_json(series);
    props["show_crosshair"] = show_crosshair;
    props["axis_x"] = chart_axis_spec_to_json(axis_x);
    props["axis_y"] = chart_axis_spec_to_json(axis_y);
    props["legend"] = chart_legend_spec_to_json(legend);
    props["padding"] = edge_insets_to_json(padding);
}

inline auto ScatterChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("series")) {
        series = json_to_scatter_series_vector(props["series"]);
    }
    if (props.contains("show_crosshair") && props["show_crosshair"].is_boolean()) {
        show_crosshair = props["show_crosshair"].get<bool>();
    }
    if (props.contains("axis_x")) {
        axis_x = json_to_chart_axis_spec(props["axis_x"]);
    }
    if (props.contains("axis_y")) {
        axis_y = json_to_chart_axis_spec(props["axis_y"]);
    }
    if (props.contains("legend")) {
        legend = json_to_chart_legend_spec(props["legend"]);
    }
    if (props.contains("padding")) {
        padding = json_to_edge_insets(props["padding"]);
    }
    mark_needs_layout();
    mark_needs_paint();
}

inline auto ScatterChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme theme = inherit_theme(ctx);
    const Font &font = theme.font;
    const Point origin = bounds.origin;
    const Geometry &g = geom_;
    const Rect &plot = g.plot;
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.text.r, theme.text.g, theme.text.b, 31};
    const Color axis = Color{theme.text.r, theme.text.g, theme.text.b, 160};

    if (axis_y.visible) {
        draw_numeric_axis(p, origin, plot, g.y_scale, axis_y, font, grid, axis);
    }
    // x 轴：把数值刻度标在绘图区下方（无类目标签，用数值刻度代替）
    if (axis_x.visible && plot.size.width > 0.0F) {
        const int digits = tick_digits(g.x_scale.step());
        for (const double t : g.x_scale.ticks()) {
            const float x = g.x_scale.to_px(t, plot.origin.x, plot.right());
            if (x < plot.origin.x - 0.5F || x > plot.right() + 0.5F) {
                continue;
            }
            if (axis_x.show_grid_lines) {
                p.draw_line(Point{.x = origin.x + x, .y = origin.y + plot.origin.y},
                            Point{.x = origin.x + x, .y = origin.y + plot.bottom()}, 1.0F, grid);
            }
            const std::string s = format_number(t, Locale{}, digits);
            const float w = render::FontEngine::measure_width(s, font);
            const Rect box{.origin = Point{.x = origin.x + x - (w * 0.5F), .y = origin.y + plot.bottom() + 2.0F},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, s, font, axis);
        }
        if (!axis_x.label.empty()) {
            const float w = render::FontEngine::measure_width(axis_x.label, font);
            const Rect box{.origin = Point{.x = origin.x + plot.origin.x + (plot.size.width * 0.5F) - (w * 0.5F),
                                           .y = origin.y + plot.bottom() + line_h + 2.0F},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, axis_x.label, font, axis);
        }
    }

    std::optional<Point> hovered_px;
    const float grow_t = static_cast<float>(grow_.progress());  // grow-in：点半径 0 → 1
    for (std::size_t k = 0; k < series.size(); ++k) {
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
        Color c = resolve_series_color(k, series[k].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        const float radius = std::max(0.5F, series[k].dot_radius * grow_t);
        for (std::size_t i = 0; i < series[k].points.size(); ++i) {
            const ChartPoint &pt = series[k].points[i];
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
                continue;
            }
            const float px = g.x_scale.to_px(pt.x, plot.origin.x, plot.right());
            const float py = g.y_scale.to_px(pt.y, plot.bottom(), plot.origin.y);
            const Rect dot{.origin = Point{.x = origin.x + px - radius, .y = origin.y + py - radius},
                           .size = Size{.width = radius * 2.0F, .height = radius * 2.0F}};
            p.fill_rounded_rect(dot, radius, c);
            if (hovered_point_.has_value() && hovered_point_->first == static_cast<int>(k) &&
                hovered_point_->second == static_cast<int>(i)) {
                hovered_px = Point{.x = px, .y = py};
            }
        }
    }

    if (legend.visible) {
        for (std::size_t k = 0; k < g.legend_rects.size() && k < series.size(); ++k) {
            const Rect &r = g.legend_rects[k];
            const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
            Color c = resolve_series_color(k, series[k].color, theme);
            if (dimmed) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = origin.x + r.origin.x, .y = origin.y + r.origin.y + 3.0F},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(swatch, 2.0F, c);
            const Rect box{.origin = Point{.x = origin.x + r.origin.x + 14.0F, .y = origin.y + r.origin.y},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(box, series[k].name, font, axis);
        }
    }

    if (hovered_px.has_value()) {
        if (show_crosshair) {
            p.draw_line(Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.origin.y},
                        Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.bottom()}, 1.0F, grid);
            p.draw_line(Point{.x = origin.x + plot.origin.x, .y = origin.y + hovered_px->y},
                        Point{.x = origin.x + plot.right(), .y = origin.y + hovered_px->y}, 1.0F, grid);
        }
        const float d = 12.0F;
        const Rect ring{.origin = Point{.x = origin.x + hovered_px->x - (d * 0.5F),
                                        .y = origin.y + hovered_px->y - (d * 0.5F)},
                        .size = Size{.width = d, .height = d}};
        p.draw_rounded_border(ring, d * 0.5F, 2.0F, theme.text);

        if (hovered_point_.has_value()) {
            const auto [k, i] = *hovered_point_;
            if (k >= 0 && static_cast<std::size_t>(k) < series.size() &&
                static_cast<std::size_t>(i) < series[static_cast<std::size_t>(k)].points.size()) {
                const ChartPoint &pt = series[static_cast<std::size_t>(k)].points[static_cast<std::size_t>(i)];
                const std::string text = series[static_cast<std::size_t>(k)].name + ": (" + std::to_string(pt.x) +
                                         ", " + std::to_string(pt.y) + ")";
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                const float bx = std::clamp(hovered_px->x + 8.0F, 0.0F, std::max(0.0F, bounds.size.width - w));
                const float by = std::clamp(hovered_px->y - h - 4.0F, 0.0F, std::max(0.0F, bounds.size.height - h));
                const Rect box{.origin = Point{.x = origin.x + bx, .y = origin.y + by},
                               .size = Size{.width = w, .height = h}};
                p.fill_rounded_rect(box, 4.0F,
                                    Color{theme.background.r, theme.background.g, theme.background.b, 242});
                p.draw_rounded_border(box, 4.0F, 1.0F, grid);
                const Rect text_box{.origin = Point{.x = origin.x + bx + 8.0F, .y = origin.y + by + 4.0F},
                                    .size = Size{.width = w - 16.0F, .height = line_h}};
                p.draw_text(text_box, text, font, theme.text);
            }
        }
    }
}

}  // namespace aurora
