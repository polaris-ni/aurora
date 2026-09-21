#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/types.h"
#include "aurora/i18n/format.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/chart_common.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief BarChart 属性（聚合；所有字段均有默认值，支持指定初始化器）。
struct BarChartProps {
    std::vector<ChartSeries> series;  ///< 数据系列（多系列分组并排；单系列即普通柱状）
    std::vector<std::string> categories;  ///< x 轴类目标签；缺省 "1","2",...
    bool stacked = false;  ///< 堆叠模式（同 x 的多系列累加）
    float bar_width_ratio = 0.7F;  ///< 柱宽在带内的占比，夹取 (0,1]
    float bar_corner_radius = 2.0F;  ///< 柱圆角（自动不超过柱宽/柱高的一半）
    bool show_crosshair = true;  ///< 悬停时是否绘制十字准线（吸附最近类目）
    ChartAxisSpec axis_x;  ///< 类目轴（Band）
    ChartAxisSpec axis_y;  ///< 数值轴（Linear）
    ChartLegendSpec legend;  ///< 图例
    EdgeInsets padding{8.0F, 8.0F, 8.0F, 8.0F};  ///< 图内留白（轴标签 / 值框避让区，D12）
};

/**
 * @brief 柱状图控件（叶控件，切片 3；契约见 specification/04-widget.md §3.8）。
 *
 * 纯值属性驱动（`BarChartProps`），数据进序列化面（D5），绘制全部经软件 `Painter`：
 * 柱体 = `fill_rounded_rect`、网格 / 轴 = `draw_line` + `draw_text`、悬停值框自绘。
 * 轴域与命中反查同源于 `LinearScale` / `BandScale`（D6），故悬停命中的类目与渲染一致。
 *
 * 三种构造形态等价（`CODING_STANDARDS.md` §11.1）：
 * @code
 *   au::BarChart(au::BarChartProps{ .series = {{ .name = "A", .values = {1, 2, 3} }} });
 *   au::BarChart().set_series({{ .name = "A", .values = {1, 2, 3} }});
 *   auto c = au::BarChart(); c.series = {{ .name = "A", .values = {1, 2, 3} }};
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class BarChart : public LeafWidget, public BarChartProps {
  public:
    BarChart() = default;
    explicit BarChart(BarChartProps props) : BarChartProps(std::move(props)) {}

    /// @brief 运行时可查询的默认属性值。
    [[nodiscard]] static auto defaults() -> BarChartProps { return BarChartProps{}; }

    /// @brief 点击（抬起）命中数据柱时触发：`(系列索引, 类目索引)`。旁挂，不进序列化面。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    // ---- 链式 setter（与 Props 直接赋值等价）----
    auto set_series(std::vector<ChartSeries> s) -> BarChart & {
        series = std::move(s);
        mark_needs_layout();
        mark_needs_paint();
        grow_.replay();  // 数据变更 → 重放入场动画（无 Animator 时为 no-op）
        return *this;
    }
    auto set_categories(std::vector<std::string> c) -> BarChart & {
        categories = std::move(c);
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }
    auto set_stacked(bool v) -> BarChart & {
        stacked = v;
        mark_needs_paint();
        return *this;
    }
    auto set_bar_width_ratio(float r) -> BarChart & {
        bar_width_ratio = r;
        mark_needs_paint();
        return *this;
    }
    auto set_bar_corner_radius(float r) -> BarChart & {
        bar_corner_radius = r;
        mark_needs_paint();
        return *this;
    }
    auto set_show_crosshair(bool v) -> BarChart & {
        show_crosshair = v;
        mark_needs_paint();
        return *this;
    }
    auto set_axis_x(ChartAxisSpec a) -> BarChart & {
        axis_x = a;
        mark_needs_layout();
        return *this;
    }
    auto set_axis_y(ChartAxisSpec a) -> BarChart & {
        axis_y = a;
        mark_needs_layout();
        return *this;
    }
    auto set_legend(ChartLegendSpec l) -> BarChart & {
        legend = l;
        mark_needs_layout();
        return *this;
    }
    auto set_padding(const EdgeInsets &p) -> BarChart & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "BarChart"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 消费指针事件：悬停高亮与命中回调（图表自带点击目标语义）。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停离开时清除高亮（基类默认只置 `hover_` 不标脏，必须覆写）。
    auto on_hover_change(bool entered) -> void override {
        hover_ = entered;
        if (!entered && (hovered_point_.has_value() || legend_hover_.has_value())) {
            hovered_point_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍标签：图类型与规模（数据本身不进语义树，避免读屏念出一长串数字）。
    /// @brief 无障碍角色：图表族统一为 `Image`（D8）—— 推断表不识 `BarChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值：当前悬停 / 选中的数据点（未悬停时为空）。
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 类目索引）；无悬停为空。测试与调试用。
    [[nodiscard]] auto hovered_point() const -> std::optional<std::pair<int, int>> { return hovered_point_; }
    /// @brief 当前悬停的图例项索引；无悬停为空。
    [[nodiscard]] auto hovered_legend() const -> std::optional<std::size_t> { return legend_hover_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

  private:
    /// @brief 布局期算定的绘图几何（局部坐标，原点 0）：渲染与命中反查共用同一份（D6）。
    struct Geometry {
        Rect plot{};  ///< 柱体绘制区（不含轴留白）
        LinearScale y_scale{};  ///< 数值轴
        BandScale x_band{0};  ///< 类目轴
        std::vector<std::string> cats;
        float band_w = 0.0F;  ///< 单类目带宽
        float group_w = 0.0F;  ///< 带内柱组总宽
        float bar_w = 0.0F;  ///< 单柱宽（堆叠时 = group_w）
        std::vector<Rect> legend_rects;  ///< 图例项命中区（局部坐标）
    };

    [[nodiscard]] auto resolved_categories() const -> std::vector<std::string>;
    [[nodiscard]] auto category_count() const -> std::size_t;
    [[nodiscard]] auto compute_y_scale() const -> LinearScale;
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    [[nodiscard]] auto hit_test_point(const Point &local) const -> std::optional<std::pair<int, int>>;
    [[nodiscard]] auto legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t>;
    [[nodiscard]] auto series_value(std::size_t series_idx, std::size_t cat_idx) const -> double;
    [[nodiscard]] auto category_center(const Geometry &g, std::size_t cat_idx) const -> float;

    Geometry geom_;
    ChartGrowIn grow_;
    std::optional<std::pair<int, int>> hovered_point_;
    std::optional<std::size_t> legend_hover_;
};

// ---------------------------------------------------------------------------
// 实现（header-only：与既有 ProgressIndicator 同形态，逻辑全部为纯几何 / 纯值计算）
// ---------------------------------------------------------------------------

inline auto BarChart::category_count() const -> std::size_t {
    std::size_t n = categories.size();
    for (const ChartSeries &s : series) {
        n = std::max(n, s.values.size());
    }
    return n;
}

inline auto BarChart::resolved_categories() const -> std::vector<std::string> {
    const std::size_t n = category_count();
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(i < categories.size() ? categories[i] : std::to_string(i + 1));
    }
    return out;
}

inline auto BarChart::series_value(std::size_t series_idx, std::size_t cat_idx) const -> double {
    if (series_idx >= series.size()) {
        return 0.0;
    }
    const std::vector<double> &vals = series[series_idx].values;
    if (cat_idx >= vals.size()) {
        return 0.0;
    }
    return std::isfinite(vals[cat_idx]) ? vals[cat_idx] : 0.0;
}

inline auto BarChart::compute_y_scale() const -> LinearScale {
    const std::size_t n_cat = category_count();
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    if (stacked) {
        // 堆叠：每个类目的有效上界 = 该类目各系列之和。
        for (std::size_t j = 0; j < n_cat; ++j) {
            double sum = 0.0;
            for (std::size_t i = 0; i < series.size(); ++i) {
                sum += series_value(i, j);
            }
            lo = have ? std::min(lo, sum) : sum;
            hi = have ? std::max(hi, sum) : sum;
            have = true;
        }
    } else {
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
    }
    if (!have) {
        lo = 0.0;
        hi = 1.0;  // 空数据：域退化为 [0,1]（D15），只画轴不画柱
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

inline auto BarChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    g.cats = resolved_categories();
    g.y_scale = compute_y_scale();
    g.x_band = BandScale{g.cats.size()};

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = padding.left;
    float top = padding.top;
    float right = std::max(padding.left, size.width - padding.right);
    float bottom = std::max(padding.top, size.height - padding.bottom);

    // 图例带（先扣，再算轴留白）
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

    // x 轴（类目标签 + 轴标题）
    if (axis_x.visible) {
        bottom -= line_h;
        if (!axis_x.label.empty()) {
            bottom -= line_h;
        }
    }
    // y 轴（刻度标签 + 轴标题）：标签宽度取决于格式化后的刻度文本
    if (axis_y.visible) {
        const double step = g.y_scale.step();
        int digits = 0;
        if (step > 0.0 && step < 1.0) {
            digits = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 6);
        }
        float widest = 0.0F;
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
    const std::size_t n_cat = g.cats.size();
    g.band_w = n_cat == 0U ? 0.0F : (g.plot.size.width / static_cast<float>(n_cat));
    const float ratio = std::clamp(bar_width_ratio, 0.05F, 1.0F);
    g.group_w = g.band_w * ratio;
    const std::size_t n_series = series.size();
    g.bar_w = (stacked || n_series == 0U) ? g.group_w : (g.group_w / static_cast<float>(n_series));

    // 图例项命中区（与绘制同一套游标推进规则；命中优先于数据区）
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

inline auto BarChart::category_center(const Geometry &g, std::size_t cat_idx) const -> float {
    return g.x_band.band_center_px(cat_idx, g.plot.origin.x, g.plot.right());
}

inline auto BarChart::legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

inline auto BarChart::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size s = c.constrain(Size{.width = c.max.width, .height = c.max.height});
    // 无界约束（infinity）下给一个可用的缺省画布，避免绘制区退化为 0 而无法自证。
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

inline auto BarChart::hit_test_point(const Point &local) const -> std::optional<std::pair<int, int>> {
    const Rect &plot = geom_.plot;
    if (plot.size.width <= 0.0F || plot.size.height <= 0.0F || geom_.cats.empty() || series.empty()) {
        return std::nullopt;
    }
    if (local.x < plot.origin.x || local.x > plot.right() || local.y < plot.origin.y || local.y > plot.bottom()) {
        return std::nullopt;
    }
    const std::size_t j = geom_.x_band.index_at(local.x, plot.origin.x, plot.right());
    std::size_t i = 0;
    if (!stacked && series.size() > 1U) {
        const float center = geom_.x_band.band_center_px(j, plot.origin.x, plot.right());
        const float group_left = center - (geom_.group_w * 0.5F);
        const float t = geom_.bar_w > 0.0F ? (local.x - group_left) / geom_.bar_w : 0.0F;
        const auto raw = static_cast<long long>(std::floor(t));
        i = (raw < 0) ? 0U : std::min(static_cast<std::size_t>(raw), series.size() - 1U);
    } else if (stacked) {
        // 堆叠：按 y 落在哪一段决定系列（自上而下取第一个包含该 y 的段）
        const float y0 = geom_.y_scale.to_px(0.0, plot.bottom(), plot.origin.y);
        for (std::size_t k = 0; k < series.size(); ++k) {
            double acc = 0.0;
            for (std::size_t m = 0; m <= k; ++m) {
                acc += series_value(m, j);
            }
            const float y_top = geom_.y_scale.to_px(acc, plot.bottom(), plot.origin.y);
            const float y_prev = geom_.y_scale.to_px(acc - series_value(k, j), plot.bottom(), plot.origin.y);
            (void)y0;
            const float lo = std::min(y_top, y_prev);
            const float hi = std::max(y_top, y_prev);
            if (local.y >= lo && local.y <= hi) {
                i = k;
                break;
            }
        }
    }
    return std::pair<int, int>{static_cast<int>(i), static_cast<int>(j)};
}

inline auto BarChart::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Move) {
        // 图例项区域优先于数据区：命中图例即联动高亮，不再解析数据点。
        const auto legend = legend_hit(geom_, e.local_position);
        const auto hit = legend.has_value() ? std::nullopt : hit_test_point(e.local_position);
        if (legend != legend_hover_ || hit != hovered_point_) {
            legend_hover_ = legend;
            hovered_point_ = hit;
            mark_needs_paint();
        }
        e.is_handled = true;
        return;
    }
    if (e.action == MouseAction::Release) {
        const auto hit = hit_test_point(e.local_position);
        if (hit.has_value() && on_point_tapped) {
            on_point_tapped(hit->first, hit->second);
        }
        e.is_handled = true;
        return;
    }
    Widget::on_pointer_event(e);
}

inline auto BarChart::accessibility_label() const -> std::string {
    return "BarChart, " + std::to_string(series.size()) + " series, " + std::to_string(category_count()) +
           " categories";
}

inline auto BarChart::accessibility_value() const -> std::string {
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const auto [i, j] = *hovered_point_;
    if (i < 0 || j < 0 || static_cast<std::size_t>(i) >= series.size()) {
        return std::string{};
    }
    return series[static_cast<std::size_t>(i)].name + ": " +
           std::to_string(series_value(static_cast<std::size_t>(i), static_cast<std::size_t>(j)));
}

inline auto BarChart::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "BarChart",
        .properties =
            {
                {.name = "series",
                 .type = "vector<ChartSeries>",
                 .default_value = "[]",
                 .required = false,
                 .note = "数据系列数组：[{name, values:[double], color?}]",
                 .json_type = "array"},
                {.name = "categories",
                 .type = "vector<string>",
                 .default_value = "[]",
                 .required = false,
                 .note = "x 轴类目标签；缺省为序号",
                 .json_type = "array"},
                {.name = "stacked",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "堆叠模式（同 x 多系列累加）",
                 .json_type = "boolean"},
                {.name = "bar_width_ratio",
                 .type = "float",
                 .default_value = "0.7",
                 .required = false,
                 .note = "柱宽在带内占比，夹取 (0,1]",
                 .json_type = "number",
                 .min_value = "0.05",
                 .max_value = "1"},
                {.name = "bar_corner_radius",
                 .type = "float",
                 .default_value = "2.0",
                 .required = false,
                 .note = "柱圆角(dp)，自动不超过柱宽/柱高一半",
                 .json_type = "number",
                 .min_value = "0"},
                {.name = "show_crosshair",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "悬停时绘制吸附最近类目的十字准线",
                 .json_type = "boolean"},
                {.name = "axis_x",
                 .type = "Json",
                 .default_value = "{}",
                 .required = false,
                 .note = "类目轴规格：{visible,label,tick_count,min,max,show_grid_lines,include_zero}",
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
                 .note = "图内留白(dp)，轴标签与值框避让区",
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
        .invariants = {"0 < bar_width_ratio <= 1", "padding >= 0"},
        .examples = {"au::BarChart(au::BarChartProps{ .series = {{ .name = \"A\", .values = {1, 2, 3} }} })"},
    };
}

inline auto BarChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["series"] = chart_series_vector_to_json(series);
    props["categories"] = string_vector_to_json(categories);
    props["stacked"] = stacked;
    props["bar_width_ratio"] = bar_width_ratio;
    props["bar_corner_radius"] = bar_corner_radius;
    props["show_crosshair"] = show_crosshair;
    props["axis_x"] = chart_axis_spec_to_json(axis_x);
    props["axis_y"] = chart_axis_spec_to_json(axis_y);
    props["legend"] = chart_legend_spec_to_json(legend);
    props["padding"] = edge_insets_to_json(padding);
}

inline auto BarChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("series")) {
        series = json_to_chart_series_vector(props["series"]);
    }
    if (props.contains("categories")) {
        categories = json_to_string_vector(props["categories"]);
    }
    if (props.contains("stacked") && props["stacked"].is_boolean()) {
        stacked = props["stacked"].get<bool>();
    }
    if (props.contains("bar_width_ratio") && props["bar_width_ratio"].is_number()) {
        bar_width_ratio = std::clamp(props["bar_width_ratio"].get<float>(), 0.05F, 1.0F);
    }
    if (props.contains("bar_corner_radius") && props["bar_corner_radius"].is_number()) {
        bar_corner_radius = std::max(0.0F, props["bar_corner_radius"].get<float>());
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

inline auto BarChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme theme = inherit_theme(ctx);
    const Font &font = theme.font;
    const float off_x = bounds.origin.x;
    const float off_y = bounds.origin.y;
    // 局部几何 → 全局绘制坐标
    auto gx = [off_x](float x) -> float { return x + off_x; };
    auto gy = [off_y](float y) -> float { return y + off_y; };
    auto grect = [&](const Rect &r) -> Rect {
        return Rect{.origin = Point{.x = gx(r.origin.x), .y = gy(r.origin.y)}, .size = r.size};
    };

    const Geometry &g = geom_;
    const Rect &plot = g.plot;
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.text.r, theme.text.g, theme.text.b, 31};  // ≈ text × 0.12
    const Color axis = Color{theme.text.r, theme.text.g, theme.text.b, 160};  // ≈ text × 0.63

    // ---- 网格线（y 刻度）----
    if (axis_y.visible && axis_y.show_grid_lines && plot.size.height > 0.0F) {
        for (const double t : g.y_scale.ticks()) {
            const float y = g.y_scale.to_px(t, plot.bottom(), plot.origin.y);
            if (y < plot.origin.y - 0.5F || y > plot.bottom() + 0.5F) {
                continue;
            }
            p.draw_line(Point{.x = gx(plot.origin.x), .y = gy(y)}, Point{.x = gx(plot.right()), .y = gy(y)}, 1.0F,
                        grid);
        }
    }

    // ---- 柱体 ----
    const std::size_t n_cat = g.cats.size();
    const std::size_t n_series = series.size();
    std::vector<double> stack_acc(n_cat, 0.0);
    std::optional<Rect> hovered_bar;
    const double grow_t = grow_.progress();  // grow-in：柱高 0 → 1（无 Animator 时恒 1）
    for (std::size_t i = 0; i < n_series; ++i) {
        Color c = resolve_series_color(i, series[i].color, theme);
        // 图例联动：命中某图例项时，其余系列降透明（0.35）
        if (legend_hover_.has_value() && (*legend_hover_ != i)) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        for (std::size_t j = 0; j < n_cat; ++j) {
            const double v = series_value(i, j) * grow_t;
            const double lo = stacked ? stack_acc[j] : 0.0;
            const double hi = stacked ? stack_acc[j] + v : v;
            if (stacked) {
                stack_acc[j] = hi;
            }
            const float y_hi = g.y_scale.to_px(hi, plot.bottom(), plot.origin.y);
            const float y_lo = g.y_scale.to_px(lo, plot.bottom(), plot.origin.y);
            const float top = std::min(y_lo, y_hi);
            const float bottom = std::max(y_lo, y_hi);
            // 夹进绘图区：域外的值不得画到轴外（D12）
            const float clipped_top = std::max(top, plot.origin.y);
            const float clipped_bottom = std::min(bottom, plot.bottom());
            if (clipped_bottom - clipped_top <= 0.0F) {
                continue;
            }
            const float center = g.x_band.band_center_px(j, plot.origin.x, plot.right());
            const float group_left = center - (g.group_w * 0.5F);
            const float x = stacked ? group_left : (group_left + (static_cast<float>(i) * g.bar_w));
            Rect bar{.origin = Point{.x = x, .y = clipped_top},
                     .size = Size{.width = g.bar_w, .height = clipped_bottom - clipped_top}};
            if (bar.origin.x < plot.origin.x) {
                const float d = plot.origin.x - bar.origin.x;
                bar.origin.x = plot.origin.x;
                bar.size.width = std::max(0.0F, bar.size.width - d);
            }
            if (bar.right() > plot.right()) {
                bar.size.width = std::max(0.0F, plot.right() - bar.origin.x);
            }
            if (bar.size.width <= 0.0F || bar.size.height <= 0.0F) {
                continue;
            }
            const float radius = std::min(bar_corner_radius, std::min(bar.size.width, bar.size.height) * 0.5F);
            if (radius > 0.0F) {
                p.fill_rounded_rect(grect(bar), radius, c);
            } else {
                p.fill_rect(grect(bar), c);
            }
            if (hovered_point_.has_value() && hovered_point_->first == static_cast<int>(i) &&
                hovered_point_->second == static_cast<int>(j)) {
                hovered_bar = bar;
            }
        }
    }

    // ---- y 轴刻度标签 ----
    if (axis_y.visible && plot.size.height > 0.0F) {
        const double step = g.y_scale.step();
        int digits = 0;
        if (step > 0.0 && step < 1.0) {
            digits = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 6);
        }
        for (const double t : g.y_scale.ticks()) {
            const float y = g.y_scale.to_px(t, plot.bottom(), plot.origin.y);
            if (y < plot.origin.y - 0.5F || y > plot.bottom() + 0.5F) {
                continue;
            }
            const std::string s = format_number(t, Locale{}, digits);
            const float w = render::FontEngine::measure_width(s, font);
            const Rect box{.origin = Point{.x = gx(plot.origin.x - w - 6.0F), .y = gy(y - (line_h * 0.5F))},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, s, font, axis);
        }
        if (!axis_y.label.empty()) {
            const float w = render::FontEngine::measure_width(axis_y.label, font);
            const Rect box{.origin = Point{.x = gx(plot.origin.x - w - 6.0F - line_h), .y = gy(plot.origin.y)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, axis_y.label, font, axis);
        }
    }

    // ---- x 轴类目标签 ----
    if (axis_x.visible && n_cat > 0U && plot.size.width > 0.0F) {
        for (std::size_t j = 0; j < n_cat; ++j) {
            const std::string &s = g.cats[j];
            const float w = render::FontEngine::measure_width(s, font);
            const float center = g.x_band.band_center_px(j, plot.origin.x, plot.right());
            const Rect box{.origin = Point{.x = gx(center - (w * 0.5F)), .y = gy(plot.bottom() + 2.0F)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, s, font, axis);
        }
        if (!axis_x.label.empty()) {
            const float w = render::FontEngine::measure_width(axis_x.label, font);
            const Rect box{.origin = Point{.x = gx(plot.origin.x + (plot.size.width * 0.5F) - (w * 0.5F)),
                                           .y = gy(plot.bottom() + line_h + 2.0F)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, axis_x.label, font, axis);
        }
    }

    // ---- 图例（命中区与绘制同源于 geom_.legend_rects）----
    if (legend.visible) {
        for (std::size_t i = 0; i < g.legend_rects.size() && i < n_series; ++i) {
            const Rect &r = g.legend_rects[i];
            Color c = resolve_series_color(i, series[i].color, theme);
            if (legend_hover_.has_value() && (*legend_hover_ != i)) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = gx(r.origin.x), .y = gy(r.origin.y + 3.0F)},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(grect(swatch), 2.0F, c);
            const Rect box{.origin = Point{.x = gx(r.origin.x + 14.0F), .y = gy(r.origin.y)},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(grect(box), series[i].name, font, axis);
        }
    }

    // ---- 悬停高亮 + 十字准线 + 自绘值框 ----
    if (hovered_bar.has_value()) {
        // 十字准线：吸附最近类目的垂直参考线
        if (show_crosshair && hovered_point_.has_value() && hovered_point_->second >= 0) {
            const float cx = category_center(g, static_cast<std::size_t>(hovered_point_->second));
            p.draw_line(Point{.x = gx(cx), .y = gy(plot.origin.y)}, Point{.x = gx(cx), .y = gy(plot.bottom())}, 1.0F,
                        grid);
        }
        p.draw_rounded_border(grect(*hovered_bar), 2.0F, 2.0F, theme.text);
        if (hovered_point_.has_value()) {
            const auto [si, ci] = *hovered_point_;
            if (si >= 0 && static_cast<std::size_t>(si) < n_series) {
                const std::string text =
                    series[static_cast<std::size_t>(si)].name + ": " +
                    std::to_string(series_value(static_cast<std::size_t>(si), static_cast<std::size_t>(ci)));
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                // 值框一律夹在控件 bounds 内（越界像素不会被脏区擦除 → 残影，D12）
                float bx = hovered_bar->origin.x + (hovered_bar->size.width * 0.5F) - (w * 0.5F);
                float by = hovered_bar->origin.y - h - 4.0F;
                bx = std::clamp(bx, 0.0F, std::max(0.0F, bounds.size.width - w));
                by = std::clamp(by, 0.0F, std::max(0.0F, bounds.size.height - h));
                const Rect box{.origin = Point{.x = bx, .y = by}, .size = Size{.width = w, .height = h}};
                p.fill_rounded_rect(grect(box), 4.0F,
                                    Color{theme.background.r, theme.background.g, theme.background.b, 242});
                p.draw_rounded_border(grect(box), 4.0F, 1.0F, grid);
                const Rect text_box{.origin = Point{.x = bx + 8.0F, .y = by + 4.0F},
                                    .size = Size{.width = w - 16.0F, .height = line_h}};
                p.draw_text(grect(text_box), text, font, theme.text);
            }
        }
    }
}

}  // namespace aurora
