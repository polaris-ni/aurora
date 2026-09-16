#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/animation/easing.h"
#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/**
 * @brief 图表公共纯值数据层（切片 2；契约见 specification/04-widget.md §3.8）。
 *
 * 本头只放**纯值类型、纯函数与比例尺**：无状态、不持有资源、可在无头环境（无 Application、
 * 无 Surface、无字体）完整单测。绘制 / 刻度生成 / 命中反查三处**同源**消费这些类型（D6），
 * 故域计算与反查必须走同一份 `LinearScale` / `BandScale`，不得各算一遍。
 *
 * 所有聚合字段均有合理默认值（CODING_STANDARDS.md §6.2）。
 * @note Thread: thread-safe (pure value types)
 * @note Side-effects: none
 * @note Rebuildable: yes, via from_json
 */

// ---------- 数据 ----------

/// @brief 显式坐标数据点（Scatter 用；Line/Bar/Sparkline 为等距，x = 索引）。
struct ChartPoint {
    double x = 0.0;
    double y = 0.0;
};

/// @brief 数据系列（Line / Bar 共用）：`values` 等距，`name` 进图例与值框。
struct ChartSeries {
    std::string name;
    std::vector<double> values;
    std::optional<Color> color;  ///< 空 = 按索引取色板（D8 / D14）
};

/// @brief 散点系列（Scatter 专用）：显式 `ChartPoint` 坐标。
struct ScatterSeries {
    std::string name;
    std::vector<ChartPoint> points;
    std::optional<Color> color;
    float dot_radius = 4.0F;
};

/// @brief 扇区（Pie 专用）：归一化占比 = value / Σvalue。
struct PieSection {
    std::string name;
    double value = 0.0;
    std::optional<Color> color;
};

// ---------- 轴与图例 ---------

/// @brief 轴规格（纯值；渲染 / 刻度 / 反查三用）。
struct ChartAxisSpec {
    bool visible = true;              ///< 是否绘制轴与刻度
    std::string label;                ///< 轴标题（可选；非空时额外占留白）
    int tick_count = 5;               ///< 期望刻度数（nice 化后可能 ±1）
    std::optional<double> min;        ///< 域下界；空 = 取数据域并 nice 化
    std::optional<double> max;        ///< 域上界；空 = 取数据域并 nice 化
    bool show_grid_lines = true;      ///< 网格线
    bool include_zero = true;         ///< 域是否必须含 0（Bar 的零基线依赖它）
};

/// @brief 图例位置。
enum class LegendPosition {
    Top,
    Bottom,
    Right,
};

/// @brief 图例规格。
struct ChartLegendSpec {
    bool visible = true;
    LegendPosition position = LegendPosition::Top;
};

// ---------- 比例尺（D3 式纯值化）----------

/// @brief 线性比例尺：domain → px，含 nice 域、nice 刻度与反查（invert）。
class LinearScale {
  public:
    LinearScale() = default;
    LinearScale(double d0, double d1, double step) : d0_(d0), d1_(d1), step_(step) {}

    /// @brief 由数据域构造并 nice 化（D3/Qt 通用做法：step ∈ {1,2,5}×10^k，上下界向 step 对齐）。
    /// 域退化（range ≤ 0）时回退 `[lo, lo + 1]`，杜绝除零 / NaN（D15）。
    [[nodiscard]] static auto from_domain(double d0, double d1, int tick_count) -> LinearScale {
        double lo = std::min(d0, d1);
        double hi = std::max(d0, d1);
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
            return LinearScale{0.0, 1.0, 1.0};
        }
        if (!(hi > lo)) {
            hi = lo + 1.0;
        }
        const int n = std::max(tick_count, 1);
        const double step = nice_number((hi - lo) / static_cast<double>(n));
        const double nice_lo = std::floor(lo / step) * step;
        double nice_hi = std::ceil(hi / step) * step;
        if (!(nice_hi > nice_lo)) {
            nice_hi = nice_lo + step;
        }
        return LinearScale{nice_lo, nice_hi, step};
    }

    /// @brief 显式域（含 `min`/`max` 覆盖）：同样 nice 化刻度，但保留调用方指定的域边界。
    [[nodiscard]] static auto from_explicit(double d0, double d1, int tick_count) -> LinearScale {
        if (!std::isfinite(d0) || !std::isfinite(d1) || !(d1 > d0)) {
            return from_domain(d0, d1, tick_count);
        }
        const int n = std::max(tick_count, 1);
        return LinearScale{d0, d1, nice_number((d1 - d0) / static_cast<double>(n))};
    }

    [[nodiscard]] auto domain() const -> std::pair<double, double> { return {d0_, d1_}; }
    [[nodiscard]] auto step() const -> double { return step_; }

    /// @brief domain 值 → 像素（px1 < px0 用于 y 轴自上而下）。
    [[nodiscard]] auto to_px(double v, float px0, float px1) const -> float {
        const double span = d1_ - d0_;
        const double t = std::abs(span) < 1e-12 ? 0.0 : (v - d0_) / span;
        return static_cast<float>(static_cast<double>(px0) + (t * (static_cast<double>(px1) - px0)));
    }

    /// @brief 像素 → domain 值（hover 命中反查；与 `to_px` 同源）。
    [[nodiscard]] auto invert(float px, float px0, float px1) const -> double {
        const double span = static_cast<double>(px1) - static_cast<double>(px0);
        const double t = std::abs(span) < 1e-12 ? 0.0 : (static_cast<double>(px) - px0) / span;
        return d0_ + (t * (d1_ - d0_));
    }

    /// @brief nice 刻度值序列（含域两端点；数量受 step 约束，最多 1001 个防病态输入）。
    [[nodiscard]] auto ticks() const -> std::vector<double> {
        std::vector<double> out;
        if (!(step_ > 0.0) || !std::isfinite(step_)) {
            return out;
        }
        const int n = static_cast<int>(std::floor((d1_ - d0_) / step_));
        const int count = std::clamp(n, 0, 1000);
        out.reserve(static_cast<std::size_t>(count) + 1);
        for (int i = 0; i <= count; ++i) {
            const double v = d0_ + (static_cast<double>(i) * step_);
            // 消除浮点累积误差造成的 -1e-17 一类刻度标签
            out.push_back(std::abs(v) < step_ * 1e-9 ? 0.0 : v);
        }
        return out;
    }

  private:
    /// @brief nice 步长（D3 `tickIncrement` 同算法）：把原始步长吸附到 {1,2,5,10}×10^k，
    ///        阈值取 √2 / √10 / √50，使 0..100 分 5 档得到 20 而非 50（后者只给出 3 个刻度）。
    [[nodiscard]] static auto nice_number(double raw_step) -> double {
        if (!(raw_step > 0.0) || !std::isfinite(raw_step)) {
            return 1.0;
        }
        const double power = std::floor(std::log10(raw_step));
        const double error = raw_step / std::pow(10.0, power);  // ∈ [1, 10)
        const double mult = (error >= std::sqrt(50.0))   ? 10.0
                            : (error >= std::sqrt(10.0)) ? 5.0
                            : (error >= std::sqrt(2.0))  ? 2.0
                                                         : 1.0;
        return mult * std::pow(10.0, power);
    }

    double d0_ = 0.0;
    double d1_ = 1.0;
    double step_ = 1.0;
};

/// @brief 带状比例尺（Bar 类目轴）：n 个类目等分带宽，取带中心；命中反查越界夹取。
class BandScale {
  public:
    explicit BandScale(std::size_t n) : n_(n) {}

    [[nodiscard]] auto count() const -> std::size_t { return n_; }

    /// @brief 单带宽度（dp）。n == 0 时为 0（调用方须先判空，D15）。
    [[nodiscard]] auto band_width(float px0, float px1) const -> float {
        if (n_ == 0U) {
            return 0.0F;
        }
        return (px1 - px0) / static_cast<float>(n_);
    }

    /// @brief 第 i 个带的中心像素（i 越界时夹取到 [0, n-1]）。
    [[nodiscard]] auto band_center_px(std::size_t i, float px0, float px1) const -> float {
        if (n_ == 0U) {
            return px0;
        }
        const std::size_t idx = std::min(i, n_ - 1U);
        const float step = (px1 - px0) / static_cast<float>(n_);
        return px0 + ((static_cast<float>(idx) + 0.5F) * step);
    }

    /// @brief 像素 → 类目索引（越界夹取，保证永远可安全下标）。
    [[nodiscard]] auto index_at(float px, float px0, float px1) const -> std::size_t {
        if (n_ == 0U) {
            return 0U;
        }
        const float step = (px1 - px0) / static_cast<float>(n_);
        if (std::abs(step) < 1e-6F) {
            return 0U;
        }
        const float t = (px - px0) / step;
        const auto raw = static_cast<long long>(std::floor(t));
        if (raw < 0) {
            return 0U;
        }
        return std::min(static_cast<std::size_t>(raw), n_ - 1U);
    }

  private:
    std::size_t n_ = 0;
};

// ---------- 色板 ----------

/// @brief 内置系列色板（Material 风格 8 色；索引超界取模）。
[[nodiscard]] inline auto chart_palette(std::size_t index) -> Color {
    constexpr Color PALETTE[8] = {
        Color{66, 133, 244, 255},   // blue
        Color{219, 68, 55, 255},    // red
        Color{244, 180, 0, 255},    // yellow
        Color{15, 157, 88, 255},    // green
        Color{171, 71, 188, 255},   // purple
        Color{255, 112, 67, 255},   // deep orange
        Color{0, 172, 193, 255},    // cyan
        Color{124, 179, 66, 255},   // light green
    };
    return PALETTE[index % 8U];
}

/// @brief 系列取色优先级（D14）：显式 color > `Theme` 命名令牌 `chart.palette.<i%8>` > 内置色板。
[[nodiscard]] inline auto resolve_series_color(std::size_t index, const std::optional<Color> &explicit_color,
                                               const Theme &theme) -> Color {
    if (explicit_color.has_value()) {
        return *explicit_color;
    }
    return theme.token_or<Color>("chart.palette." + std::to_string(index % 8U), chart_palette(index));
}

// ---------- JSON 编解码（数据进序列化面，D5）----------

/// @brief `vector<double>` → JSON 数组（非数值元素跳过）。
[[nodiscard]] inline auto double_vector_to_json(const std::vector<double> &v) -> Json {
    Json a = Json::array();
    for (const double x : v) {
        a.push_back(std::isfinite(x) ? x : 0.0);
    }
    return a;
}

/// @brief JSON 数组 → `vector<double>`（非数组 / 非数值元素跳过，绝不抛异常）。
[[nodiscard]] inline auto json_to_double_vector(const Json &j) -> std::vector<double> {
    std::vector<double> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const Json &item : j) {
        if (item.is_number()) {
            const double v = item.get<double>();
            out.push_back(std::isfinite(v) ? v : 0.0);
        }
    }
    return out;
}

/// @brief 字符串数组 ↔ JSON（类目标签）。
[[nodiscard]] inline auto string_vector_to_json(const std::vector<std::string> &v) -> Json {
    Json a = Json::array();
    for (const std::string &s : v) {
        a.push_back(s);
    }
    return a;
}

[[nodiscard]] inline auto json_to_string_vector(const Json &j) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const Json &item : j) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

/// @brief 单个系列 → JSON 对象（`color` 未设置时不输出，保留「按索引取色板」语义）。
[[nodiscard]] inline auto chart_series_to_json(const ChartSeries &s) -> Json {
    Json o = Json::object();
    o["name"] = s.name;
    o["values"] = double_vector_to_json(s.values);
    if (s.color.has_value()) {
        o["color"] = color_to_json(*s.color);
    }
    return o;
}

/// @brief JSON 对象 → 系列（字段缺失 / 类型不符逐项回退默认，绝不抛异常）。
[[nodiscard]] inline auto json_to_chart_series(const Json &j) -> ChartSeries {
    ChartSeries s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j["name"].is_string()) {
        s.name = j["name"].get<std::string>();
    }
    if (j.contains("values")) {
        s.values = json_to_double_vector(j["values"]);
    }
    if (j.contains("color") && j["color"].is_array()) {
        s.color = json_to_color(j["color"]);
    }
    return s;
}

/// @brief 对象数组（`ChartSeries`）↔ JSON。
[[nodiscard]] inline auto chart_series_vector_to_json(const std::vector<ChartSeries> &v) -> Json {
    Json a = Json::array();
    for (const ChartSeries &s : v) {
        a.push_back(chart_series_to_json(s));
    }
    return a;
}

[[nodiscard]] inline auto json_to_chart_series_vector(const Json &j) -> std::vector<ChartSeries> {
    std::vector<ChartSeries> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const Json &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_chart_series(item));
        }
    }
    return out;
}

/// @brief 散点系列 ↔ JSON。
[[nodiscard]] inline auto scatter_series_to_json(const ScatterSeries &s) -> Json {
    Json o = Json::object();
    o["name"] = s.name;
    Json pts = Json::array();
    for (const ChartPoint &pt : s.points) {
        pts.push_back(Json::array({std::isfinite(pt.x) ? pt.x : 0.0, std::isfinite(pt.y) ? pt.y : 0.0}));
    }
    o["points"] = std::move(pts);
    o["dot_radius"] = s.dot_radius;
    if (s.color.has_value()) {
        o["color"] = color_to_json(*s.color);
    }
    return o;
}

[[nodiscard]] inline auto json_to_scatter_series(const Json &j) -> ScatterSeries {
    ScatterSeries s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j["name"].is_string()) {
        s.name = j["name"].get<std::string>();
    }
    if (j.contains("dot_radius") && j["dot_radius"].is_number()) {
        s.dot_radius = j["dot_radius"].get<float>();
    }
    if (j.contains("color") && j["color"].is_array()) {
        s.color = json_to_color(j["color"]);
    }
    if (j.contains("points") && j["points"].is_array()) {
        for (const Json &item : j["points"]) {
            if (item.is_array() && item.size() >= 2 && item[0].is_number() && item[1].is_number()) {
                s.points.push_back(ChartPoint{.x = item[0].get<double>(), .y = item[1].get<double>()});
            }
        }
    }
    return s;
}

[[nodiscard]] inline auto scatter_series_vector_to_json(const std::vector<ScatterSeries> &v) -> Json {
    Json a = Json::array();
    for (const ScatterSeries &s : v) {
        a.push_back(scatter_series_to_json(s));
    }
    return a;
}

[[nodiscard]] inline auto json_to_scatter_series_vector(const Json &j) -> std::vector<ScatterSeries> {
    std::vector<ScatterSeries> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const Json &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_scatter_series(item));
        }
    }
    return out;
}

/// @brief 扇区 ↔ JSON。
[[nodiscard]] inline auto pie_section_to_json(const PieSection &s) -> Json {
    Json o = Json::object();
    o["name"] = s.name;
    o["value"] = std::isfinite(s.value) ? s.value : 0.0;
    if (s.color.has_value()) {
        o["color"] = color_to_json(*s.color);
    }
    return o;
}

[[nodiscard]] inline auto json_to_pie_section(const Json &j) -> PieSection {
    PieSection s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j["name"].is_string()) {
        s.name = j["name"].get<std::string>();
    }
    if (j.contains("value") && j["value"].is_number()) {
        const double v = j["value"].get<double>();
        s.value = std::isfinite(v) ? v : 0.0;
    }
    if (j.contains("color") && j["color"].is_array()) {
        s.color = json_to_color(j["color"]);
    }
    return s;
}

[[nodiscard]] inline auto pie_section_vector_to_json(const std::vector<PieSection> &v) -> Json {
    Json a = Json::array();
    for (const PieSection &s : v) {
        a.push_back(pie_section_to_json(s));
    }
    return a;
}

[[nodiscard]] inline auto json_to_pie_section_vector(const Json &j) -> std::vector<PieSection> {
    std::vector<PieSection> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const Json &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_pie_section(item));
        }
    }
    return out;
}

// ---------- 枚举编解码 ----------

/// @brief LegendPosition → JSON 字符串。
[[nodiscard]] inline auto legend_position_to_json(LegendPosition v) -> Json {
    switch (v) {
        case LegendPosition::Top:
            return "Top";
        case LegendPosition::Bottom:
            return "Bottom";
        case LegendPosition::Right:
            return "Right";
    }
    return "Top";
}

/// @brief JSON → LegendPosition（未知值回退 Top）。
[[nodiscard]] inline auto json_to_legend_position(const Json &j) -> LegendPosition {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Bottom") {
            return LegendPosition::Bottom;
        }
        if (s == "Right") {
            return LegendPosition::Right;
        }
    }
    return LegendPosition::Top;
}

/// @brief 轴规格 ↔ JSON（嵌套对象；缺失字段回退默认）。
[[nodiscard]] inline auto chart_axis_spec_to_json(const ChartAxisSpec &a) -> Json {
    Json o = Json::object();
    o["visible"] = a.visible;
    if (!a.label.empty()) {
        o["label"] = a.label;
    }
    o["tick_count"] = a.tick_count;
    if (a.min.has_value()) {
        o["min"] = *a.min;
    }
    if (a.max.has_value()) {
        o["max"] = *a.max;
    }
    o["show_grid_lines"] = a.show_grid_lines;
    o["include_zero"] = a.include_zero;
    return o;
}

[[nodiscard]] inline auto json_to_chart_axis_spec(const Json &j) -> ChartAxisSpec {
    ChartAxisSpec a;
    if (!j.is_object()) {
        return a;
    }
    if (j.contains("visible") && j["visible"].is_boolean()) {
        a.visible = j["visible"].get<bool>();
    }
    if (j.contains("label") && j["label"].is_string()) {
        a.label = j["label"].get<std::string>();
    }
    if (j.contains("tick_count") && j["tick_count"].is_number()) {
        a.tick_count = std::max(2, j["tick_count"].get<int>());
    }
    if (j.contains("min") && j["min"].is_number()) {
        a.min = j["min"].get<double>();
    }
    if (j.contains("max") && j["max"].is_number()) {
        a.max = j["max"].get<double>();
    }
    if (j.contains("show_grid_lines") && j["show_grid_lines"].is_boolean()) {
        a.show_grid_lines = j["show_grid_lines"].get<bool>();
    }
    if (j.contains("include_zero") && j["include_zero"].is_boolean()) {
        a.include_zero = j["include_zero"].get<bool>();
    }
    return a;
}

/// @brief 图例规格 ↔ JSON。
[[nodiscard]] inline auto chart_legend_spec_to_json(const ChartLegendSpec &l) -> Json {
    Json o = Json::object();
    o["visible"] = l.visible;
    o["position"] = legend_position_to_json(l.position);
    return o;
}

[[nodiscard]] inline auto json_to_chart_legend_spec(const Json &j) -> ChartLegendSpec {
    ChartLegendSpec l;
    if (!j.is_object()) {
        return l;
    }
    if (j.contains("visible") && j["visible"].is_boolean()) {
        l.visible = j["visible"].get<bool>();
    }
    if (j.contains("position")) {
        l.position = json_to_legend_position(j["position"]);
    }
    return l;
}

// ---------- grow-in 动画载荷（切片 8）----------

/// @brief 图表入场动画（grow-in）：控制器 + 进度 `State<double>` + 帧循环注册/注销。
///
/// 进度写入 `State<double>`，随 `collect_signals` 参与响应式刷新，故动画不另起通道。
/// **无运行中 `Animator`（`Animator::current() == nullptr`，典型为 `render_to_png` / golden）时
/// 进度恒为 1**——终态降级，保证无头渲染输出确定（D11）。`reduce_motion` 由
/// `AnimationController::tick` 统一短路，本类无需特判。
///
/// 用法：控件持有一个成员，`on_mount` 调 `mount()`，数据变更调 `replay()`，绘制读 `progress()`。
class ChartGrowIn {
  public:
    ChartGrowIn() = default;
    ~ChartGrowIn() {
        if (bound_) {
            if (Animator *a = Animator::current()) {
                a->remove(ctrl_);
            }
            bound_ = false;
        }
    }
    ChartGrowIn(const ChartGrowIn &) = delete;
    auto operator=(const ChartGrowIn &) -> ChartGrowIn & = delete;
    auto operator=(ChartGrowIn &&) -> ChartGrowIn & = delete;

    /// @brief 移动构造：控件经 `Node{widget}` 入树必须可移动。
    ///
    /// 绑定建立在 `on_mount`（入树之后，此后不再移动），故此处若已登记则先摘除再转移进度值——
    /// 绝不让 `Animator` 持有已失效的控制器 / 目标地址（`Animator::drive` 存裸指针，UAF 风险）。
    ChartGrowIn(ChartGrowIn &&other) noexcept : progress_{other.progress_.get()} {
        if (other.bound_) {
            if (Animator *a = Animator::current()) {
                a->remove(other.ctrl_);
            }
            other.bound_ = false;
        }
        other.progress_.set(1.0);
    }

    /// @brief 当前进度（0..1）；未接动画时恒为 1（终态）。
    [[nodiscard]] auto progress() const -> double { return progress_.get(); }
    /// @brief 进度信号（供 `collect_signals` 登记）。
    [[nodiscard]] auto signal() -> State<double> & { return progress_; }
    /// @brief 是否正在播放（用于 `can_cache_display_list()`）。
    [[nodiscard]] auto animating() const -> bool { return bound_ && ctrl_.is_animating(); }

    /// @brief 在 `on_mount` 中调用：接上帧循环并从头播放；无 Animator 时落到终态。
    auto mount() -> void {
        Animator *a = Animator::current();
        if (a == nullptr) {
            progress_.set(1.0);  // 降级：无运行循环（无头渲染）直接呈现终态
            return;
        }
        progress_.set(0.0);
        a->bind(ctrl_, Tween<double>{0.0, 1.0, Curves::ease_out()}, progress_);
        bound_ = true;
        ctrl_.forward();
    }

    /// @brief 数据变更时重放（未接动画时为 no-op，进度已恒为终态）。
    auto replay() -> void {
        if (!bound_) {
            progress_.set(1.0);
            return;
        }
        progress_.set(0.0);
        ctrl_.forward();
    }

  private:
    AnimationController ctrl_{0.45};  ///< 450ms 入场
    State<double> progress_{1.0};     ///< 初值为终态（未播放 = 已完成）
    bool bound_ = false;              ///< 是否已登记进 Animator（决定析构是否摘除）
};

// ---------- 轴绘制共享实现（Bar / Line / Scatter 共用，避免四份漂移）----------

/// @brief 刻度小数位：由 nice step 推导（step ≥ 1 → 0 位）。
[[nodiscard]] inline auto tick_digits(double step) -> int {
    if (!(step > 0.0) || !std::isfinite(step) || step >= 1.0) {
        return 0;
    }
    return std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 6);
}

/// @brief 数值轴（y）绘制：网格线 + 刻度标签 + 轴标题。
///
/// `origin` 为控件在画布中的全局原点（几何一律用局部坐标，绘制时才平移），
/// 保证「渲染与命中同源」。`plot` 为局部坐标下的绘图区。
inline auto draw_numeric_axis(Painter &p, Point origin, const Rect &plot, const LinearScale &scale,
                              const ChartAxisSpec &spec, const Font &font, Color grid_color, Color text_color) -> void {
    if (plot.size.height <= 0.0F) {
        return;
    }
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const int digits = tick_digits(scale.step());
    for (const double t : scale.ticks()) {
        const float y = scale.to_px(t, plot.bottom(), plot.origin.y);
        if (y < plot.origin.y - 0.5F || y > plot.bottom() + 0.5F) {
            continue;
        }
        if (spec.show_grid_lines) {
            p.draw_line(Point{.x = origin.x + plot.origin.x, .y = origin.y + y},
                        Point{.x = origin.x + plot.right(), .y = origin.y + y}, 1.0F, grid_color);
        }
        const std::string s = format_number(t, Locale{}, digits);
        const float w = render::FontEngine::measure_width(s, font);
        const Rect box{.origin = Point{.x = origin.x + plot.origin.x - w - 6.0F, .y = origin.y + y - (line_h * 0.5F)},
                       .size = Size{.width = w + 2.0F, .height = line_h}};
        p.draw_text(box, s, font, text_color);
    }
    if (!spec.label.empty()) {
        const float w = render::FontEngine::measure_width(spec.label, font);
        const Rect box{.origin = Point{.x = origin.x + plot.origin.x - w - 6.0F - line_h, .y = origin.y + plot.origin.y},
                       .size = Size{.width = w + 2.0F, .height = line_h}};
        p.draw_text(box, spec.label, font, text_color);
    }
}

/// @brief 类目轴（x）绘制：带中心标签 + 轴标题。`cats` 为空时直接返回。
inline auto draw_category_axis(Painter &p, Point origin, const Rect &plot, const BandScale &band,
                               const std::vector<std::string> &cats, const ChartAxisSpec &spec, const Font &font,
                               Color text_color) -> void {
    if (cats.empty() || plot.size.width <= 0.0F) {
        return;
    }
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    for (std::size_t j = 0; j < cats.size(); ++j) {
        const std::string &s = cats[j];
        const float w = render::FontEngine::measure_width(s, font);
        const float center = band.band_center_px(j, plot.origin.x, plot.right());
        const Rect box{.origin = Point{.x = origin.x + center - (w * 0.5F), .y = origin.y + plot.bottom() + 2.0F},
                       .size = Size{.width = w + 2.0F, .height = line_h}};
        p.draw_text(box, s, font, text_color);
    }
    if (!spec.label.empty()) {
        const float w = render::FontEngine::measure_width(spec.label, font);
        const Rect box{.origin = Point{.x = origin.x + plot.origin.x + (plot.size.width * 0.5F) - (w * 0.5F),
                                       .y = origin.y + plot.bottom() + line_h + 2.0F},
                       .size = Size{.width = w + 2.0F, .height = line_h}};
        p.draw_text(box, spec.label, font, text_color);
    }
}

/// @brief 扇区归一化占比（Σ ≤ 0 时返回全 0，由调用方按 D15 降级处理）。
[[nodiscard]] inline auto pie_section_ratios(const std::vector<PieSection> &sections) -> std::vector<double> {
    std::vector<double> out(sections.size(), 0.0);
    double total = 0.0;
    for (const PieSection &s : sections) {
        if (std::isfinite(s.value) && s.value > 0.0) {
            total += s.value;
        }
    }
    if (!(total > 0.0)) {
        return out;
    }
    for (std::size_t i = 0; i < sections.size(); ++i) {
        out[i] = (std::isfinite(sections[i].value) && sections[i].value > 0.0) ? sections[i].value / total : 0.0;
    }
    return out;
}

}  // namespace aurora
