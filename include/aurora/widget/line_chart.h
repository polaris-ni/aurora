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

namespace aurora {

/// @brief LineChart 属性（聚合；所有字段均有默认值）。
struct LineChartProps {
    std::vector<ChartSeries> series;      ///< 数据系列（`values` 等距，x = 索引）
    std::vector<std::string> categories;  ///< x 轴类目标签；缺省 "1","2",...
    bool show_dots = true;                ///< 是否绘制数据点圆点
    float line_width = 2.0F;              ///< 折线宽度（dp）
    float dot_radius = 3.0F;              ///< 数据点半径（dp）
    bool show_crosshair = true;           ///< 悬停时是否绘制十字准线（吸附最近数据 x）
    ChartAxisSpec axis_x;                 ///< 类目轴
    ChartAxisSpec axis_y;                 ///< 数值轴（Linear）
    ChartLegendSpec legend;
    EdgeInsets padding{8.0F, 8.0F, 8.0F, 8.0F};
};

/**
 * @brief 折线图控件（叶控件，切片 4；契约见 specification/04-widget.md §3.8）。
 *
 * 折线经 Painter 的 `stroke_polyline`（真 SDF，圆角连接），数据点用圆角矩形复用圆形。
 * x 为等距索引（无独立 x 域），y 域与命中反查同源于 `LinearScale`（D6）。
 * 悬停按「最近数据点欧氏距离」命中（阈值 = `dot_radius + 8dp`），并叠加吸附该点的十字准线。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class LineChart : public LeafWidget, public LineChartProps {
  public:
    LineChart() = default;
    explicit LineChart(LineChartProps props) : LineChartProps(std::move(props)) {}

    [[nodiscard]] static auto defaults() -> LineChartProps { return LineChartProps{}; }

    /// @brief 点击（抬起）命中数据点时触发：`(系列索引, 点索引)`。旁挂，不进序列化面。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    auto set_series(std::vector<ChartSeries> s) -> LineChart & {
        series = std::move(s);
        mark_needs_layout();
        mark_needs_paint();
        grow_.replay();
        return *this;
    }
    auto set_categories(std::vector<std::string> c) -> LineChart & {
        categories = std::move(c);
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }
    auto set_show_dots(bool v) -> LineChart & {
        show_dots = v;
        mark_needs_paint();
        return *this;
    }
    auto set_line_width(float w) -> LineChart & {
        line_width = w;
        mark_needs_paint();
        return *this;
    }
    auto set_dot_radius(float r) -> LineChart & {
        dot_radius = r;
        mark_needs_paint();
        return *this;
    }
    auto set_axis_x(ChartAxisSpec a) -> LineChart & {
        axis_x = a;
        mark_needs_layout();
        return *this;
    }
    auto set_axis_y(ChartAxisSpec a) -> LineChart & {
        axis_y = a;
        mark_needs_layout();
        return *this;
    }
    auto set_legend(ChartLegendSpec l) -> LineChart & {
        legend = l;
        mark_needs_layout();
        return *this;
    }
    auto set_padding(const EdgeInsets &p) -> LineChart & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "LineChart"; }

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

    [[nodiscard]] auto accessibility_label() const -> std::string override;
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 点索引）；无悬停为空。
    [[nodiscard]] auto hovered_point() const -> std::optional<std::pair<int, int>> { return hovered_point_; }
    /// @brief 当前悬停的图例项索引；无悬停为空。
    [[nodiscard]] auto hovered_legend() const -> std::optional<std::size_t> { return legend_hover_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

  private:
    struct Geometry {
        Rect plot{};
        LinearScale y_scale{};
        std::vector<std::string> cats;
        std::size_t point_count = 0;
        std::vector<Rect> legend_rects;  ///< 图例项命中区（局部坐标）
    };

    [[nodiscard]] auto point_count() const -> std::size_t;
    [[nodiscard]] auto resolved_categories() const -> std::vector<std::string>;
    [[nodiscard]] auto compute_y_scale() const -> LinearScale;
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    [[nodiscard]] auto point_x(const Geometry &g, std::size_t i) const -> float;
    [[nodiscard]] auto legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t>;
    [[nodiscard]] auto nearest_point(const Geometry &g, const Point &local) const
        -> std::optional<std::pair<int, int>>;
    [[nodiscard]] auto series_value(std::size_t series_idx, std::size_t point_idx) const -> double;

    Geometry geom_;
    ChartGrowIn grow_;
    std::optional<std::pair<int, int>> hovered_point_;
    std::optional<std::size_t> legend_hover_;
};

inline auto LineChart::point_count() const -> std::size_t {
    std::size_t n = categories.size();
    for (const ChartSeries &s : series) {
        n = std::max(n, s.values.size());
    }
    return n;
}

inline auto LineChart::resolved_categories() const -> std::vector<std::string> {
    const std::size_t n = point_count();
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(i < categories.size() ? categories[i] : std::to_string(i + 1));
    }
    return out;
}

inline auto LineChart::series_value(std::size_t series_idx, std::size_t point_idx) const -> double {
    if (series_idx >= series.size()) {
        return 0.0;
    }
    const std::vector<double> &vals = series[series_idx].values;
    if (point_idx >= vals.size()) {
        return 0.0;
    }
    return std::isfinite(vals[point_idx]) ? vals[point_idx] : 0.0;
}

inline auto LineChart::compute_y_scale() const -> LinearScale {
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    for (const ChartSeries &s : series) {
        for (const double v : s.values) {
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
    if (axis_y.min.has_value()) {
        lo = *axis_y.min;
    }
    if (axis_y.max.has_value()) {
        hi = *axis_y.max;
    }
    if (axis_y.include_zero) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    if (axis_y.min.has_value() && axis_y.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, axis_y.tick_count);
    }
    return LinearScale::from_domain(lo, hi, axis_y.tick_count);
}

inline auto LineChart::point_x(const Geometry &g, std::size_t i) const -> float {
    if (g.point_count <= 1U) {
        return g.plot.origin.x + (g.plot.size.width * 0.5F);
    }
    const float t = static_cast<float>(i) / static_cast<float>(g.point_count - 1U);
    return g.plot.origin.x + (t * g.plot.size.width);
}

inline auto LineChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    g.cats = resolved_categories();
    g.point_count = g.cats.size();
    g.y_scale = compute_y_scale();

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = padding.left;
    float top = padding.top;
    float right = std::max(padding.left, size.width - padding.right);
    float bottom = std::max(padding.top, size.height - padding.bottom);

    // 图例带 + 命中区（图例项区域优先于数据区）
    if (legend.visible && !series.empty()) {
        if (legend.position == LegendPosition::Top) {
            top += line_h;
        } else if (legend.position == LegendPosition::Bottom) {
            bottom -= line_h;
        } else {
            float widest = 0.0F;
            for (const ChartSeries &s : series) {
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

    // 图例项命中区（与绘制同一套游标推进规则）
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
        for (const ChartSeries &s : series) {
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

inline auto LineChart::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
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

inline auto LineChart::legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

inline auto LineChart::nearest_point(const Geometry &g, const Point &local) const
    -> std::optional<std::pair<int, int>> {
    if (g.plot.size.width <= 0.0F || g.plot.size.height <= 0.0F || g.point_count == 0U || series.empty()) {
        return std::nullopt;
    }
    const float threshold = dot_radius + 8.0F;
    float best = threshold;
    std::optional<std::pair<int, int>> best_hit;
    for (std::size_t k = 0; k < series.size(); ++k) {
        for (std::size_t i = 0; i < g.point_count; ++i) {
            const float px = point_x(g, i);
            const float py = g.y_scale.to_px(series_value(k, i), g.plot.bottom(), g.plot.origin.y);
            const float dx = local.x - px;
            const float dy = local.y - py;
            const float d = std::sqrt((dx * dx) + (dy * dy));
            if (d <= best) {
                best = d;
                best_hit = std::pair<int, int>{static_cast<int>(k), static_cast<int>(i)};
            }
        }
    }
    return best_hit;
}

inline auto LineChart::on_pointer_event(MouseEvent &e) -> void {
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

inline auto LineChart::accessibility_label() const -> std::string {
    return "LineChart, " + std::to_string(series.size()) + " series, " + std::to_string(point_count()) + " points";
}

inline auto LineChart::accessibility_value() const -> std::string {
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const auto [k, i] = *hovered_point_;
    if (k < 0 || i < 0 || static_cast<std::size_t>(k) >= series.size()) {
        return std::string{};
    }
    return series[static_cast<std::size_t>(k)].name + ": " +
           std::to_string(series_value(static_cast<std::size_t>(k), static_cast<std::size_t>(i)));
}

inline auto LineChart::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "LineChart",
        .properties =
            {
                {.name = "series",
                 .type = "vector<ChartSeries>",
                 .default_value = "[]",
                 .required = false,
                 .note = "数据系列数组（values 等距，x = 索引）",
                 .json_type = "array"},
                {.name = "categories",
                 .type = "vector<string>",
                 .default_value = "[]",
                 .required = false,
                 .note = "x 轴类目标签；缺省为序号",
                 .json_type = "array"},
                {.name = "show_dots",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "是否绘制数据点圆点",
                 .json_type = "boolean"},
                {.name = "line_width",
                 .type = "float",
                 .default_value = "2.0",
                 .required = false,
                 .note = "折线宽度(dp)",
                 .json_type = "number",
                 .min_value = "0"},
                {.name = "dot_radius",
                 .type = "float",
                 .default_value = "3.0",
                 .required = false,
                 .note = "数据点半径(dp)",
                 .json_type = "number",
                 .min_value = "0"},
                {.name = "show_crosshair",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "悬停时绘制吸附最近数据点的十字准线",
                 .json_type = "boolean"},
                {.name = "axis_x",
                 .type = "Json",
                 .default_value = "{}",
                 .required = false,
                 .note = "类目轴规格：{visible,label,tick_count,show_grid_lines}",
                 .json_type = "object"},
                {.name = "axis_y",
                 .type = "Json",
                 .default_value = "{}",
                 .required = false,
                 .note = "数值轴规格：{visible,label,tick_count,min,max,show_grid_lines,include_zero}",
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
        .invariants = {"line_width >= 0", "dot_radius >= 0"},
        .examples = {"au::LineChart(au::LineChartProps{ .series = {{ .name = \"A\", .values = {1, 3, 2} }} })"},
    };
}

inline auto LineChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["series"] = chart_series_vector_to_json(series);
    props["categories"] = string_vector_to_json(categories);
    props["show_dots"] = show_dots;
    props["line_width"] = line_width;
    props["dot_radius"] = dot_radius;
    props["show_crosshair"] = show_crosshair;
    props["axis_x"] = chart_axis_spec_to_json(axis_x);
    props["axis_y"] = chart_axis_spec_to_json(axis_y);
    props["legend"] = chart_legend_spec_to_json(legend);
    props["padding"] = edge_insets_to_json(padding);
}

inline auto LineChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("series")) {
        series = json_to_chart_series_vector(props["series"]);
    }
    if (props.contains("categories")) {
        categories = json_to_string_vector(props["categories"]);
    }
    if (props.contains("show_dots") && props["show_dots"].is_boolean()) {
        show_dots = props["show_dots"].get<bool>();
    }
    if (props.contains("line_width") && props["line_width"].is_number()) {
        line_width = std::max(0.0F, props["line_width"].get<float>());
    }
    if (props.contains("dot_radius") && props["dot_radius"].is_number()) {
        dot_radius = std::max(0.0F, props["dot_radius"].get<float>());
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

inline auto LineChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
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

    // ---- 折线 + 数据点（含图例联动降透明）----
    const double grow_t = grow_.progress();  // grow-in：描线进度（无 Animator 时恒 1）
    std::optional<Point> hovered_px;
    for (std::size_t k = 0; k < series.size(); ++k) {
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
        Color c = resolve_series_color(k, series[k].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        std::vector<Point> pts;
        pts.reserve(g.point_count);
        for (std::size_t i = 0; i < g.point_count; ++i) {
            const float px = point_x(g, i);
            const float py = g.y_scale.to_px(series_value(k, i), plot.bottom(), plot.origin.y);
            pts.push_back(Point{.x = origin.x + px, .y = origin.y + py});
            if (hovered_point_.has_value() && hovered_point_->first == static_cast<int>(k) &&
                hovered_point_->second == static_cast<int>(i)) {
                hovered_px = Point{.x = px, .y = py};
            }
        }
        // grow-in：只绘制到 t 对应的折线前缀（末段按分数插值端点）
        std::vector<Point> drawn = pts;
        std::size_t dot_count = pts.size();
        if (grow_t < 1.0 && pts.size() >= 2) {
            const float span = static_cast<float>(pts.size() - 1) * static_cast<float>(grow_t);
            const std::size_t whole = static_cast<std::size_t>(std::floor(span));
            const float frac = span - static_cast<float>(whole);
            const std::size_t keep = std::min(whole + 1U, pts.size());
            drawn.assign(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(keep));
            if (keep < pts.size() && frac > 0.0F) {
                const Point &a = pts[keep - 1U];
                const Point &b = pts[keep];
                drawn.push_back(Point{.x = a.x + ((b.x - a.x) * frac), .y = a.y + ((b.y - a.y) * frac)});
            }
            dot_count = keep;
        }
        if (drawn.size() >= 2 && line_width > 0.0F) {
            p.stroke_polyline(drawn, line_width, c);
        }
        if (show_dots && dot_radius > 0.0F) {
            const float d = dot_radius * 2.0F;
            for (std::size_t idx = 0; idx < dot_count && idx < pts.size(); ++idx) {
                const Point &pt = pts[idx];
                const Rect dot{.origin = Point{.x = pt.x - dot_radius, .y = pt.y - dot_radius},
                               .size = Size{.width = d, .height = d}};
                p.fill_rounded_rect(dot, dot_radius, c);
            }
        }
    }

    // ---- 类目轴 ----
    if (axis_x.visible) {
        BandScale band{g.point_count};
        draw_category_axis(p, origin, plot, band, g.cats, axis_x, font, axis);
    }

    // ---- 图例 ----
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

    // ---- 十字准线 + 悬停高亮 + 值框 ----
    if (hovered_px.has_value()) {
        if (show_crosshair) {
            p.draw_line(Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.origin.y},
                        Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.bottom()}, 1.0F, grid);
        }
        const float d = std::max(4.0F, dot_radius * 2.0F) + 4.0F;
        const Rect ring{.origin = Point{.x = origin.x + hovered_px->x - (d * 0.5F),
                                        .y = origin.y + hovered_px->y - (d * 0.5F)},
                        .size = Size{.width = d, .height = d}};
        p.draw_rounded_border(ring, d * 0.5F, 2.0F, theme.text);

        if (hovered_point_.has_value()) {
            const auto [k, i] = *hovered_point_;
            if (k >= 0 && static_cast<std::size_t>(k) < series.size()) {
                const std::string text =
                    series[static_cast<std::size_t>(k)].name + ": " +
                    std::to_string(series_value(static_cast<std::size_t>(k), static_cast<std::size_t>(i)));
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                float bx = hovered_px->x + 8.0F;
                float by = hovered_px->y - h - 4.0F;
                bx = std::clamp(bx, 0.0F, std::max(0.0F, bounds.size.width - w));
                by = std::clamp(by, 0.0F, std::max(0.0F, bounds.size.height - h));
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
