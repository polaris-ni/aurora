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

/// @brief ScatterChart 属性（聚合；所有字段均有默认值）。
struct ScatterChartProps {
    std::vector<ScatterSeries> series;  ///< 散点系列（显式 `ChartPoint` 坐标）
    bool show_crosshair = true;  ///< 悬停时是否绘制十字准线（吸附最近点 x）
    ChartAxisSpec axis_x;  ///< x 轴（Linear）
    ChartAxisSpec axis_y;  ///< y 轴（Linear）
    ChartLegendSpec legend;  ///< 图例规格（可见性与位置）
    EdgeInsets padding{.left = 8.0F, .top = 8.0F, .right = 8.0F, .bottom = 8.0F};  ///< 图内留白（dp）
};

/// @brief 散点图控件（叶控件，切片 6；契约见 specification/04-widget.md §3.8）。
///
/// x / y 双 `LinearScale`（域由数据推导，可经 `axis_*.min/max` 覆盖），圆点绘制。
/// 命中按**最近点欧氏距离**（阈值 = `dot_radius + 4dp`），与渲染同源于同一组比例尺（D6）。
///
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class ScatterChart : public LeafWidget, public ScatterChartProps {
  public:
    /// @brief 默认构造：全部属性取 ScatterChartProps 默认值（无系列、十字准线开）。
    ScatterChart() = default;
    /// @brief 以属性聚合构造（移动存入基类子对象）。
    /// @param props 散点图属性（系列、轴规格、准线开关、图例、留白等）。
    explicit ScatterChart(ScatterChartProps props) : ScatterChartProps(std::move(props)) {}

    /// @brief 属性聚合的默认值（工厂便捷入口）。
    /// @return 默认构造的 ScatterChartProps。
    [[nodiscard]] static auto defaults() -> ScatterChartProps { return ScatterChartProps{}; }

    /// @brief 点击（抬起）命中数据点时触发：`(系列索引, 点索引)`。旁挂，不进序列化面。
    /// @param series_idx 回调收到的系列索引（0 基）。
    /// @param point_idx 回调收到的点索引（系列内 0 基）。
    /// @return 回调函数对象；缺省未挂接（空 std::function）。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    /// @brief 替换散点系列并重播 grow-in 动画。
    /// @param s 新系列列表（各含点名与显式坐标点集）。
    /// @return ScatterChart 引用（链式调用）。
    auto set_series(std::vector<ScatterSeries> s) -> ScatterChart & {
        series = std::move(s);
        mark_needs_layout();
        mark_needs_paint();
        grow_.replay();
        return *this;
    }
    /// @brief 设置 x 轴规格（可见性/刻度数/显式 min-max/网格线）。
    /// @param a x 轴 ChartAxisSpec。
    /// @return ScatterChart 引用（链式调用）。
    auto set_axis_x(ChartAxisSpec a) -> ScatterChart & {
        axis_x = std::move(a);
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置 y 轴规格（可见性/刻度数/显式 min-max/网格线）。
    /// @param a y 轴 ChartAxisSpec。
    /// @return ScatterChart 引用（链式调用）。
    auto set_axis_y(ChartAxisSpec a) -> ScatterChart & {
        axis_y = std::move(a);
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置图例规格（可见性 / Top|Bottom|Right 位置）。
    /// @param l 图例规格。
    /// @return ScatterChart 引用（链式调用）。
    auto set_legend(ChartLegendSpec l) -> ScatterChart & {
        legend = l;
        mark_needs_layout();
        return *this;
    }
    /// @brief 设置图内留白。
    /// @param p 四边留白（dp）。
    /// @return ScatterChart 引用（链式调用）。
    auto set_padding(const EdgeInsets &p) -> ScatterChart & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "ScatterChart"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "ScatterChart"; }

    /// @brief 静态自描述表：Inspector 用的属性/事件/不变量/示例元数据。
    /// @return WidgetDescriptor，含 width/height/show 公共属性与 on_point_tapped 事件。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（属性/事件/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的响应式信号到构建面。
    /// @param out 输出收集向量（追加 grow_ 动画进度信号的基类指针）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&grow_.signal()); }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    /// @return grow_ 动画进行中为 false，可缓存时为 true。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    /// @brief 序列化系列/准线开关/双轴/图例/留白，先经基类写公共属性。
    /// @param props 输出目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从属性 JSON 回填系列/准线开关/双轴/图例/留白：先经基类回填公共属性，类型不符或缺失的键保持
    ///        当前值，完成后请求重排重绘。
    /// @param props 输入 JSON 对象。
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 声明接收点击事件（数据点轻触回调依赖）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停态变化：离开时清空数据点/图例悬停并重绘。
    /// @param entered 指针进入为 true，离开为 false。
    auto on_hover_change(bool entered) -> void override {
        hover_ = entered;
        if (!entered && (hovered_point_.has_value() || legend_hover_.has_value())) {
            hovered_point_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    /// @brief 鼠标事件：Move 先图例后最近点更新悬停态（仅状态变化才重绘）；Release 命中数据点且回调已挂则触发
    ///        on_point_tapped。
    /// @param e 指针事件。
    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`（D8）—— 推断表不识 `ScatterChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @return 恒为 AccessibilityRole::Image。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    /// @brief 无障碍标签：控件名与系列数、总点数。
    /// @return "ScatterChart, N series, M points" 文本（M 为各系列点数之和）。
    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值文本：悬停数据点的系列名与坐标。
    /// @return 悬停且索引合法时为 "name: (x, y)"，无悬停或索引越界为空串。
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 点索引）；无悬停为空。
    /// @return hovered_point_ 的快照（nullopt = 无悬停）。
    [[nodiscard]] auto hovered_point() const -> std::optional<std::pair<int, int>> { return hovered_point_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    /// @brief 绘制坐标轴、网格、数据点、图例与悬停准线/值框：点半径随 grow-in 进度由 0 长到 1，悬停系列外的其余
    ///        系列减淡，悬停点加圆环标记，十字准线受 show_crosshair 控制。
    /// @param p 绘制器
    /// @param bounds 绘制边界
    /// @param ctx 构建上下文（取主题色/字体）
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    /// @param ctx 构建上下文（当前实现未使用）。
    auto on_mount([[maybe_unused]] const BuildContext &ctx) -> void override { grow_.mount(); }

  private:
    struct Geometry {
        Rect plot{};  ///< 绘图区矩形（轴/图例占位与留白扣除后的可用区）
        LinearScale x_scale;  ///< x 轴线性比例尺（域来自 domain_of(true)）
        LinearScale y_scale;  ///< y 轴线性比例尺（绘制时 bottom→top 反向映射）
        std::vector<Rect> legend_rects;  ///< 各图例项的命中矩形（与系列序对齐）
    };

    /// @brief 指定轴的数据域：全体有限点的 min/max 并集，可被轴规格 min/max 覆盖。
    /// @param x_axis true 取 x 轴、false 取 y 轴
    /// @return {lo, hi}；无有效数据时为 {0, 1}
    [[nodiscard]] auto domain_of(bool x_axis) const -> std::pair<double, double>;
    /// @brief 指定轴的比例尺：min/max 皆显式给定走 from_explicit，否则 from_domain（nice 步长）。
    /// @param x_axis true 取 x 轴、false 取 y 轴
    /// @return LinearScale（含 tick_count 对应刻度数）
    [[nodiscard]] auto scale_of(bool x_axis) const -> LinearScale;
    /// @brief 依尺寸与字体计算几何（绘图区扣减、比例尺与图例排布）。
    /// @param size 当前布局尺寸
    /// @param font 主题字体（量取图例文本宽高）
    /// @return Geometry 快照（on_layout 时存入 geom_）
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    /// @brief 图例项矩形命中判定。
    /// @param g 几何快照
    /// @param local 本地坐标
    /// @return 命中图例项序号，无命中为 nullopt
    [[nodiscard]] auto legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t>;
    /// @brief 最近数据点命中：比例尺映射后按各系列阈值 dot_radius + 4dp 比较欧氏距离，命中多个时取最近。
    /// @param g 几何快照
    /// @param local 本地坐标
    /// @return 命中点 (系列索引, 点索引)；绘图区退化或无命中为 nullopt
    [[nodiscard]] auto nearest_point(const Geometry &g, const Point &local) const -> std::optional<std::pair<int, int>>;

    Geometry geom_;  ///< 当前几何（每次 on_layout 重算）
    ChartGrowIn grow_;  ///< grow-in 动画器（on_mount 接入帧循环）
    std::optional<std::pair<int, int>> hovered_point_;  ///< 当前悬停数据点（nullopt = 无）
    std::optional<std::size_t> legend_hover_;  ///< 当前悬停图例项（nullopt = 无）
};

// 非有限坐标跳过；首个有限点确定 lo/hi 初值，之后逐项收窄。
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

// 域取自 domain_of，刻度数沿用轴规格的 tick_count。
inline auto ScatterChart::scale_of(bool x_axis) const -> LinearScale {
    const auto [lo, hi] = domain_of(x_axis);
    const ChartAxisSpec &spec = x_axis ? axis_x : axis_y;
    if (spec.min.has_value() && spec.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, spec.tick_count);
    }
    return LinearScale::from_domain(lo, hi, spec.tick_count);
}

// 绘图区依次扣减留白、图例占位与两轴的刻度/标签占位，再按图例位置逐项排布命中矩形。
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
        // Right（含未知位置）沿用绘图区顶部，即下面 else 分支不再重复赋同一值——
        // 重复赋值会让本初值在三条互斥分支下都永不被读（死存储）。
        float cursor_y = g.plot.origin.y;
        if (legend.position == LegendPosition::Top) {
            cursor_y = padding.top;
        } else if (legend.position == LegendPosition::Bottom) {
            cursor_y = size.height - padding.bottom - line_h;
        } else {
            cursor_x = g.plot.right() + 8.0F;
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

/// @brief 布局：按约束定尺寸（非有限约束回退 300×200），并重算 geom_。
/// @param c 父级约束
/// @param ctx 构建上下文（继承主题字体）
/// @return 本帧控件尺寸（宽高均夹到非负）
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

// 顺序扫描 legend_rects，首个包含该点的矩形即命中。
inline auto ScatterChart::legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

// 非有限坐标的点跳过；阈值按所属系列的 dot_radius 计，逐项与当前最近者比较。
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

// Move/Release 以外的动作回落基类处理。
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

// 总点数为各系列 points.size() 之和。
inline auto ScatterChart::accessibility_label() const -> std::string {
    std::size_t total = 0;
    for (const ScatterSeries &s : series) {
        total += s.points.size();
    }
    return "ScatterChart, " + std::to_string(series.size()) + " series, " + std::to_string(total) + " points";
}

// 系列索引与点索引均校验负值与越界，任一越界即返回空串。
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

// 属性表与 ScatterChartProps 字段一一对应，另附基类的 width/height/show。
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
                 .default_value = R"({"visible":true,"position":"Top"})",
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

// 键集与 describe_static 的属性表一致。
inline auto ScatterChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["series"] = scatter_series_vector_to_json(series);
    props["show_crosshair"] = show_crosshair;
    props["axis_x"] = chart_axis_spec_to_json(axis_x);
    props["axis_y"] = chart_axis_spec_to_json(axis_y);
    props["legend"] = chart_legend_spec_to_json(legend);
    props["padding"] = edge_insets_to_json(padding);
}

// 键名与 serialize_props 对称；series/轴/图例/留白交由转换函数直接覆盖。
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

// 绘制顺序：y 轴与网格 → x 轴数值刻度 → 数据点 → 图例 → 悬停准线/圆环/值框。
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
    const auto grow_t = static_cast<float>(grow_.progress());  // grow-in：点半径 0 → 1
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
            if (hovered_point_.has_value() && std::cmp_equal(hovered_point_->first, k) &&
                std::cmp_equal(hovered_point_->second, i)) {
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
        const Rect ring{
            .origin = Point{.x = origin.x + hovered_px->x - (d * 0.5F), .y = origin.y + hovered_px->y - (d * 0.5F)},
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
                p.fill_rounded_rect(box, 4.0F, Color{theme.background.r, theme.background.g, theme.background.b, 242});
                p.draw_rounded_border(box, 4.0F, 1.0F, grid);
                const Rect text_box{.origin = Point{.x = origin.x + bx + 8.0F, .y = origin.y + by + 4.0F},
                                    .size = Size{.width = w - 16.0F, .height = line_h}};
                p.draw_text(text_box, text, font, theme.text);
            }
        }
    }
}

}  // namespace aurora
