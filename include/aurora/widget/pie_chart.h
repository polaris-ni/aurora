#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/i18n/format.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/chart_common.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief PieChart 属性（聚合；所有字段均有默认值）。
struct PieChartProps {
    std::vector<PieSection> sections;  ///< 扇区；占比 = value / Σvalue
    float center_space_ratio = 0.0F;   ///< 内径 / 外径比；> 0 即 donut
    float start_angle = -90.0F;        ///< 起始角（度，0 = +x 方向；-90 = 12 点钟）
    bool show_percentage_labels = false;  ///< 扇区内百分比文本
    float section_gap = 2.0F;          ///< 扇区间隙（dp，按角度换算后两侧各让出一半）
    ChartLegendSpec legend;
    EdgeInsets padding{8.0F, 8.0F, 8.0F, 8.0F};
};

/**
 * @brief 饼图 / 环图控件（叶控件，切片 5；契约见 specification/04-widget.md §3.8）。
 *
 * 扇区经 `fill_sector` 绘制（`center_space_ratio > 0` 即环图），间隙以角度让位实现（非描边）。
 * 命中使用**极坐标**判定：先按半径落在 `[inner, outer]`，再按角度定位扇区（与渲染同源）。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class PieChart : public LeafWidget, public PieChartProps {
  public:
    PieChart() = default;
    explicit PieChart(PieChartProps props) : PieChartProps(std::move(props)) {}

    [[nodiscard]] static auto defaults() -> PieChartProps { return PieChartProps{}; }

    /// @brief 点击（抬起）命中扇区时触发：`(扇区索引)`。旁挂，不进序列化面。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int section_idx)> on_section_tapped;

    auto set_sections(std::vector<PieSection> s) -> PieChart & {
        sections = std::move(s);
        mark_needs_layout();
        mark_needs_paint();
        grow_.replay();
        return *this;
    }
    auto set_center_space_ratio(float r) -> PieChart & {
        center_space_ratio = std::clamp(r, 0.0F, 0.95F);
        mark_needs_paint();
        return *this;
    }
    auto set_start_angle(float deg) -> PieChart & {
        start_angle = deg;
        mark_needs_paint();
        return *this;
    }
    auto set_show_percentage_labels(bool v) -> PieChart & {
        show_percentage_labels = v;
        mark_needs_paint();
        return *this;
    }
    auto set_legend(ChartLegendSpec l) -> PieChart & {
        legend = l;
        mark_needs_layout();
        return *this;
    }
    auto set_padding(const EdgeInsets &p) -> PieChart & {
        padding = p;
        mark_needs_layout();
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "PieChart"; }

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
        if (!entered && (hovered_section_.has_value() || legend_hover_.has_value())) {
            hovered_section_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    auto on_pointer_event(MouseEvent &e) -> void override;

    [[nodiscard]] auto accessibility_label() const -> std::string override;
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的扇区索引；无悬停为空。
    [[nodiscard]] auto hovered_section() const -> std::optional<int> { return hovered_section_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态，D11）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

  private:
    struct Geometry {
        Rect plot{};
        Point center{};
        float outer = 0.0F;
        float inner = 0.0F;
        std::vector<double> ratios;
        std::vector<Rect> legend_rects;
    };

    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    [[nodiscard]] auto section_hit(const Geometry &g, const Point &local) const -> std::optional<int>;
    [[nodiscard]] auto legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t>;

    Geometry geom_;
    ChartGrowIn grow_;
    std::optional<int> hovered_section_;
    std::optional<std::size_t> legend_hover_;
};

inline auto PieChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    g.ratios = pie_section_ratios(sections);
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = padding.left;
    float top = padding.top;
    float right = std::max(padding.left, size.width - padding.right);
    float bottom = std::max(padding.top, size.height - padding.bottom);

    if (legend.visible && !sections.empty()) {
        if (legend.position == LegendPosition::Top) {
            top += line_h;
        } else if (legend.position == LegendPosition::Bottom) {
            bottom -= line_h;
        } else {
            float widest = 0.0F;
            for (const PieSection &s : sections) {
                widest = std::max(widest, render::FontEngine::measure_width(s.name, font));
            }
            right = std::max(left, right - (widest + 22.0F));
        }
    }
    g.plot = Rect{.origin = Point{.x = left, .y = top},
                  .size = Size{.width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)}};
    g.center = Point{.x = g.plot.origin.x + (g.plot.size.width * 0.5F),
                     .y = g.plot.origin.y + (g.plot.size.height * 0.5F)};
    g.outer = std::min(g.plot.size.width, g.plot.size.height) * 0.5F;
    g.inner = g.outer * std::clamp(center_space_ratio, 0.0F, 0.95F);

    if (legend.visible && !sections.empty()) {
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
        for (const PieSection &s : sections) {
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

inline auto PieChart::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size s = c.constrain(Size{.width = c.max.width, .height = c.max.height});
    if (!std::isfinite(c.max.width)) {
        s.width = 240.0F;
    }
    if (!std::isfinite(c.max.height)) {
        s.height = 200.0F;
    }
    s.width = std::max(s.width, 0.0F);
    s.height = std::max(s.height, 0.0F);
    geom_ = compute_geometry(s, inherit_theme(ctx).font);
    return s;
}

inline auto PieChart::legend_hit(const Geometry &g, const Point &local) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

inline auto PieChart::section_hit(const Geometry &g, const Point &local) const -> std::optional<int> {
    if (g.outer <= 0.0F || g.ratios.empty()) {
        return std::nullopt;
    }
    const float dx = local.x - g.center.x;
    const float dy = local.y - g.center.y;
    const float r = std::sqrt((dx * dx) + (dy * dy));
    if (r < g.inner || r > g.outer) {
        return std::nullopt;
    }
    constexpr float TWO_PI = 6.28318530717958647692F;
    constexpr float DEG = 0.01745329251994329577F;
    float ang = std::atan2(dy, dx) - (start_angle * DEG);
    ang = std::fmod(ang, TWO_PI);
    if (ang < 0.0F) {
        ang += TWO_PI;
    }
    float cursor = 0.0F;
    for (std::size_t i = 0; i < g.ratios.size(); ++i) {
        const float sweep = static_cast<float>(g.ratios[i]) * TWO_PI;
        if (sweep <= 0.0F) {
            continue;
        }
        if (ang >= cursor && ang < cursor + sweep) {
            return static_cast<int>(i);
        }
        cursor += sweep;
    }
    return std::nullopt;
}

inline auto PieChart::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Move) {
        const auto legend = legend_hit(geom_, e.local_position);
        const auto section = legend.has_value() ? std::nullopt : section_hit(geom_, e.local_position);
        if (legend != legend_hover_ || section != hovered_section_) {
            legend_hover_ = legend;
            hovered_section_ = section;
            mark_needs_paint();
        }
        e.is_handled = true;
        return;
    }
    if (e.action == MouseAction::Release) {
        if (const auto section = section_hit(geom_, e.local_position); section.has_value() && on_section_tapped) {
            on_section_tapped(*section);
        }
        e.is_handled = true;
        return;
    }
    Widget::on_pointer_event(e);
}

inline auto PieChart::accessibility_label() const -> std::string {
    return "PieChart, " + std::to_string(sections.size()) + " sections";
}

inline auto PieChart::accessibility_value() const -> std::string {
    if (!hovered_section_.has_value() || *hovered_section_ < 0 ||
        static_cast<std::size_t>(*hovered_section_) >= sections.size()) {
        return std::string{};
    }
    const std::size_t i = static_cast<std::size_t>(*hovered_section_);
    return sections[i].name + ": " + std::to_string(sections[i].value);
}

inline auto PieChart::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "PieChart",
        .properties =
            {
                {.name = "sections",
                 .type = "vector<PieSection>",
                 .default_value = "[]",
                 .required = false,
                 .note = "扇区数组：[{name, value, color?}]，占比 = value / Σvalue",
                 .json_type = "array"},
                {.name = "center_space_ratio",
                 .type = "float",
                 .default_value = "0.0",
                 .required = false,
                 .note = "内径 / 外径比；> 0 即环图，夹取 [0,0.95]",
                 .json_type = "number",
                 .min_value = "0",
                 .max_value = "0.95"},
                {.name = "start_angle",
                 .type = "float",
                 .default_value = "-90.0",
                 .required = false,
                 .note = "起始角（度，0 = +x 方向；-90 = 12 点钟）",
                 .json_type = "number"},
                {.name = "show_percentage_labels",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "扇区内百分比文本",
                 .json_type = "boolean"},
                {.name = "section_gap",
                 .type = "float",
                 .default_value = "2.0",
                 .required = false,
                 .note = "扇区间隙(dp)",
                 .json_type = "number",
                 .min_value = "0"},
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
        .events = {"on_section_tapped"},
        .children_policy = "none",
        .invariants = {"0 <= center_space_ratio <= 0.95"},
        .examples = {"au::PieChart(au::PieChartProps{ .sections = {{ .name = \"A\", .value = 3 }, "
                     "{ .name = \"B\", .value = 1 }} })"},
    };
}

inline auto PieChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["sections"] = pie_section_vector_to_json(sections);
    props["center_space_ratio"] = center_space_ratio;
    props["start_angle"] = start_angle;
    props["show_percentage_labels"] = show_percentage_labels;
    props["section_gap"] = section_gap;
    props["legend"] = chart_legend_spec_to_json(legend);
    props["padding"] = edge_insets_to_json(padding);
}

inline auto PieChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("sections")) {
        sections = json_to_pie_section_vector(props["sections"]);
    }
    if (props.contains("center_space_ratio") && props["center_space_ratio"].is_number()) {
        center_space_ratio = std::clamp(props["center_space_ratio"].get<float>(), 0.0F, 0.95F);
    }
    if (props.contains("start_angle") && props["start_angle"].is_number()) {
        start_angle = props["start_angle"].get<float>();
    }
    if (props.contains("show_percentage_labels") && props["show_percentage_labels"].is_boolean()) {
        show_percentage_labels = props["show_percentage_labels"].get<bool>();
    }
    if (props.contains("section_gap") && props["section_gap"].is_number()) {
        section_gap = std::max(0.0F, props["section_gap"].get<float>());
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

inline auto PieChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme theme = inherit_theme(ctx);
    const Font &font = theme.font;
    const Point origin = bounds.origin;
    const Geometry &g = geom_;
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.text.r, theme.text.g, theme.text.b, 31};
    const Color axis = Color{theme.text.r, theme.text.g, theme.text.b, 160};

    constexpr float TWO_PI = 6.28318530717958647692F;
    constexpr float DEG = 0.01745329251994329577F;
    if (g.outer <= 0.0F || g.ratios.empty()) {
        return;
    }
    // 间隙按外半径换算成角度，两侧各让出一半
    const float gap_rad = g.outer > 0.0F ? (section_gap / g.outer) : 0.0F;
    const float grow_t = static_cast<float>(grow_.progress());  // grow-in：扇形按 t 展开总角度
    float cursor = start_angle * DEG;
    for (std::size_t i = 0; i < g.ratios.size(); ++i) {
        const float sweep = static_cast<float>(g.ratios[i]) * TWO_PI * grow_t;
        if (sweep <= gap_rad) {
            cursor += sweep;
            continue;
        }
        const float a0 = cursor + (gap_rad * 0.5F);
        const float a1 = cursor + sweep - (gap_rad * 0.5F);
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != i);
        Color c = resolve_series_color(i, sections[i].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        p.fill_sector(Point{.x = origin.x + g.center.x, .y = origin.y + g.center.y}, g.outer, g.inner, a0, a1, c);
        if (hovered_section_.has_value() && *hovered_section_ == static_cast<int>(i)) {
            p.stroke_arc(Point{.x = origin.x + g.center.x, .y = origin.y + g.center.y},
                         (g.outer + g.inner) * 0.5F, std::max(1.0F, g.outer - g.inner), a0, a1, theme.text);
        }
        if (show_percentage_labels && g.ratios[i] > 0.0F) {
            const float mid = (a0 + a1) * 0.5F;
            const float r = (g.inner + g.outer) * 0.5F;
            const std::string text = format_number(g.ratios[i] * 100.0, Locale{}, 0) + "%";
            const float w = render::FontEngine::measure_width(text, font);
            const float tx = g.center.x + (std::cos(mid) * r) - (w * 0.5F);
            const float ty = g.center.y + (std::sin(mid) * r) - (line_h * 0.5F);
            // 标签一律夹在控件内（D12）
            const float cx = std::clamp(tx, 0.0F, std::max(0.0F, bounds.size.width - w));
            const float cy = std::clamp(ty, 0.0F, std::max(0.0F, bounds.size.height - line_h));
            const Rect box{.origin = Point{.x = origin.x + cx, .y = origin.y + cy},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, text, font, axis);
        }
        cursor += sweep;
    }

    // ---- 图例 ----
    if (legend.visible) {
        for (std::size_t i = 0; i < g.legend_rects.size() && i < sections.size(); ++i) {
            const Rect &r = g.legend_rects[i];
            const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != i);
            Color c = resolve_series_color(i, sections[i].color, theme);
            if (dimmed) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = origin.x + r.origin.x, .y = origin.y + r.origin.y + 3.0F},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(swatch, 2.0F, c);
            const Rect box{.origin = Point{.x = origin.x + r.origin.x + 14.0F, .y = origin.y + r.origin.y},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(box, sections[i].name, font, axis);
        }
    }

    // ---- 悬停值框 ----
    if (hovered_section_.has_value() && *hovered_section_ >= 0 &&
        static_cast<std::size_t>(*hovered_section_) < sections.size()) {
        const std::size_t i = static_cast<std::size_t>(*hovered_section_);
        const std::string text = sections[i].name + ": " + std::to_string(sections[i].value);
        const float w = render::FontEngine::measure_width(text, font) + 16.0F;
        const float h = line_h + 8.0F;
        const float bx = std::clamp(g.center.x + g.outer * 0.5F, 0.0F, std::max(0.0F, bounds.size.width - w));
        const float by = std::clamp(g.center.y - g.outer - h - 4.0F, 0.0F, std::max(0.0F, bounds.size.height - h));
        const Rect box{.origin = Point{.x = origin.x + bx, .y = origin.y + by}, .size = Size{.width = w, .height = h}};
        p.fill_rounded_rect(box, 4.0F, Color{theme.background.r, theme.background.g, theme.background.b, 242});
        p.draw_rounded_border(box, 4.0F, 1.0F, grid);
        const Rect text_box{.origin = Point{.x = origin.x + bx + 8.0F, .y = origin.y + by + 4.0F},
                            .size = Size{.width = w - 16.0F, .height = line_h}};
        p.draw_text(text_box, text, font, theme.text);
    }
}

}  // namespace aurora