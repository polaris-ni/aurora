#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
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

/// @brief 图表公共纯值数据层（契约见 specification/04-widget.md §3.8）。
///
/// 本头只放**纯值类型、纯函数与比例尺**：无状态、不持有资源、可在无头环境（无 Application、
/// 无 Surface、无字体）完整单测。绘制 / 刻度生成 / 命中反查三处**同源**消费这些类型，
/// 故域计算与反查必须走同一份 `LinearScale` / `BandScale`，不得各算一遍。
///
/// 所有聚合字段均有合理默认值（CODING_STANDARDS.md §6.2）。
/// @note Thread: thread-safe (pure value types)
/// @note Side-effects: none
/// @note Rebuildable: yes, via from_json
namespace aurora {

// ---------- 数据 ----------

/// @brief 显式坐标数据点（Scatter 用；Line/Bar/Sparkline 为等距，x = 索引）。
struct ChartPoint {
    double x = 0.0;  ///< 横坐标（数据域单位，非像素）。
    double y = 0.0;  ///< 纵坐标（数据域单位，非像素）。
};

/// @brief 数据系列（Line / Bar 共用）：`values` 等距，`name` 进图例与值框。
struct ChartSeries {
    std::string name;  ///< 系列名（图例与 hover 值框的标签；可为空串）。
    std::vector<double> values;  ///< 等距采样的数值序列（第 i 项对应 x = i）。
    std::optional<Color> color;  ///< 空 = 按索引取色板
};

/// @brief 散点系列（Scatter 专用）：显式 `ChartPoint` 坐标。
struct ScatterSeries {
    std::string name;  ///< 系列名（图例标签；可为空串）。
    std::vector<ChartPoint> points;  ///< 显式坐标点序列（x/y 均为数据域单位）。
    std::optional<Color> color;  ///< 空 = 按索引取色板。
    float dot_radius = 4.0F;  ///< 散点半径（dp），默认 4.0。
};

/// @brief 扇区（Pie 专用）：归一化占比 = value / Σvalue。
struct PieSection {
    std::string name;  ///< 扇区名（图例标签；可为空串）。
    double value = 0.0;  ///< 原始数值；仅有限且 > 0 的项参与占比计算（见 pie_section_ratios）。
    std::optional<Color> color;  ///< 空 = 按索引取色板。
};

// ---------- 轴与图例 ---------

/// @brief 轴规格（纯值；渲染 / 刻度 / 反查三用）。
struct ChartAxisSpec {
    bool visible = true;  ///< 是否绘制轴与刻度
    std::string label;  ///< 轴标题（可选；非空时额外占留白）
    int tick_count = 5;  ///< 期望刻度数（nice 化后可能 ±1）
    std::optional<double> min;  ///< 域下界；空 = 取数据域并 nice 化
    std::optional<double> max;  ///< 域上界；空 = 取数据域并 nice 化
    bool show_grid_lines = true;  ///< 网格线
    bool include_zero = true;  ///< 域是否必须含 0（Bar 的零基线依赖它）
};

// 公共 API 枚举（specification/04-widget.md 记录为数据层类型，且随 JSON 按名序列化）：
// 底层类型属 API 形态的一部分，本库按语义选型而非体积取向，改窄只让 ChartLegendSpec 少 6 字节。
/// @brief 图例位置。
/// NOLINTNEXTLINE(performance-enum-size)
enum class LegendPosition {
    Top,  ///< 顶部：图例横排于绘图区上方，行高先从留白扣除。
    Bottom,  ///< 底部：图例横排于绘图区下方。
    Right,  ///< 右侧：图例纵排于右方，按最长条目名加色块宽扣除宽度。
};

/// @brief 图例规格。
struct ChartLegendSpec {
    bool visible = true;  ///< 是否绘制图例。
    LegendPosition position = LegendPosition::Top;  ///< 图例摆放位置（默认顶部）。
};

// ---------- 比例尺（d3-scale 式纯值化）----------

/// @brief 线性比例尺：domain → px，含 nice 域、nice 刻度与反查（invert）。
class LinearScale {
  public:
    /// @brief 默认构造：域 [0, 1]、刻度步长 1。
    LinearScale() = default;
    /// @brief 以显式域与步长直接构造（不经 nice 化，通常传入 `from_domain` / `from_explicit` 的结果）。
    /// @param d0 域下界。
    /// @param d1 域上界。
    /// @param step 刻度步长（须 > 0，否则 `ticks` 返回空序列）。
    LinearScale(double d0, double d1, double step) : d0_(d0), d1_(d1), step_(step) {}

    /// @brief 由数据域构造并 nice 化（d3-scale 与 Qt 的通用做法：step ∈ {1,2,5}×10^k，上下界向 step 对齐）。
    /// 域退化（range ≤ 0）时回退 `[lo, lo + 1]`，杜绝除零 / NaN。
    /// @param d0 数据域一端（可与 d1 倒序，内部取 min/max）。
    /// @param d1 数据域另一端。
    /// @param tick_count 期望刻度数（内部 clamp ≥ 1，用于推导原始步长）。
    /// @return nice 化后的比例尺（域端点向 step 对齐，步长为 nice 数）。
    [[nodiscard]] static auto from_domain(double d0, double d1, int tick_count) -> LinearScale {
        double lo = std::min(d0, d1);  // 域两端排序后的下界。
        double hi = std::max(d0, d1);  // 域两端排序后的上界。
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
            return LinearScale{0.0, 1.0, 1.0};
        }
        if (!(hi > lo)) {
            hi = lo + 1.0;
        }
        const int n = std::max(tick_count, 1);
        const double step = nice_number((hi - lo) / static_cast<double>(n));
        const double nice_lo = std::floor(lo / step) * step;
        double nice_hi = std::ceil(hi / step) * step;  // 上界向 step 上对齐后的 nice 域上界。
        if (!(nice_hi > nice_lo)) {
            nice_hi = nice_lo + step;
        }
        return LinearScale{nice_lo, nice_hi, step};
    }

    /// @brief 显式域（含 `min`/`max` 覆盖）：同样 nice 化刻度，但保留调用方指定的域边界。
    /// @param d0 域下界（须有限且 < d1，否则整体回退 `from_domain` 的 nice 化行为）。
    /// @param d1 域上界。
    /// @param tick_count 期望刻度数（内部 clamp ≥ 1，仅用于推导步长）。
    /// @return 域保持 [d0, d1]、步长为 nice 化结果的比例尺。
    [[nodiscard]] static auto from_explicit(double d0, double d1, int tick_count) -> LinearScale {
        if (!std::isfinite(d0) || !std::isfinite(d1) || !(d1 > d0)) {
            return from_domain(d0, d1, tick_count);
        }
        const int n = std::max(tick_count, 1);
        return LinearScale{d0, d1, nice_number((d1 - d0) / static_cast<double>(n))};
    }

    /// @brief 当前域访问器（nice 化后的端点对）。
    /// @return (域下界, 域上界)。
    [[nodiscard]] auto domain() const -> std::pair<double, double> { return {d0_, d1_}; }
    /// @brief 刻度步长访问器。
    /// @return nice 化步长（`ticks` / `tick_digits` 共用）。
    [[nodiscard]] auto step() const -> double { return step_; }

    /// @brief domain 值 → 像素（px1 < px0 用于 y 轴自上而下）。
    /// @param v domain 坐标值。
    /// @param px0 像素轴起点（`v = d0` 对应位置）。
    /// @param px1 像素轴终点（`v = d1` 对应位置）。
    /// @return 线性插值后的像素坐标；域跨度退化（≤ 1e-12）时按 t = 0 处理，返回 px0。
    [[nodiscard]] auto to_px(double v, float px0, float px1) const -> float {
        const double span = d1_ - d0_;
        const double t = std::abs(span) < 1e-12 ? 0.0 : (v - d0_) / span;
        return static_cast<float>(static_cast<double>(px0) + (t * (static_cast<double>(px1) - px0)));
    }

    /// @brief 像素 → domain 值（hover 命中反查；与 `to_px` 同源）。
    /// @param px 待反查的像素坐标。
    /// @param px0 像素轴起点（与 `to_px` 传入值一致）。
    /// @param px1 像素轴终点（与 `to_px` 传入值一致）。
    /// @return 对应 domain 值；像素跨度退化（≤ 1e-12）时按 t = 0 处理，返回 d0。
    [[nodiscard]] auto invert(float px, float px0, float px1) const -> double {
        const double span = static_cast<double>(px1) - static_cast<double>(px0);
        const double t = std::abs(span) < 1e-12 ? 0.0 : (static_cast<double>(px) - px0) / span;
        return d0_ + (t * (d1_ - d0_));
    }

    /// @brief nice 刻度值序列（含域两端点；数量受 step 约束，最多 1001 个防病态输入）。
    /// @return 自域下界起、按 step 递增的刻度值序列；step 非正或非有限时为空序列。
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
    /// @brief nice 步长（与 d3-scale 的 `tickIncrement` 同算法）：把原始步长吸附到 {1,2,5,10}×10^k，
    ///        阈值取 √2 / √10 / √50，使 0..100 分 5 档得到 20 而非 50（后者只给出 3 个刻度）。
    [[nodiscard]] static auto nice_number(double raw_step) -> double {
        if (!(raw_step > 0.0) || !std::isfinite(raw_step)) {
            return 1.0;
        }
        const double power = std::floor(std::log10(raw_step));
        const double error = raw_step / std::pow(10.0, power);  // ∈ [1, 10)
        const double mult = (error >= std::sqrt(50.0))                 ? 10.0
                            : (error >= std::sqrt(10.0))               ? 5.0
                            : (error >= std::numbers::sqrt2_v<double>) ? 2.0
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
    /// @brief 以类目数构造（仅记录个数，像素参数在查询时传入）。
    /// @param n 类目数量（0 = 空轴，各查询接口按降级口径返回）。
    explicit BandScale(std::size_t n) : n_(n) {}

    /// @brief 类目数量。
    /// @return 构造时传入的 n（无类目时为 0）。
    [[nodiscard]] auto count() const -> std::size_t { return n_; }

    /// @brief 单带宽度（dp）。n == 0 时为 0（调用方须先判空）。
    /// @param px0 轴像素起点。
    /// @param px1 轴像素终点。
    /// @return (px1 - px0) / n；n == 0 时恒为 0。
    [[nodiscard]] auto band_width(float px0, float px1) const -> float {
        if (n_ == 0U) {
            return 0.0F;
        }
        return (px1 - px0) / static_cast<float>(n_);
    }

    /// @brief 第 i 个带的中心像素（i 越界时夹取到 [0, n-1]）。
    /// @param i 类目下标（越界安全：内部夹取）。
    /// @param px0 轴像素起点。
    /// @param px1 轴像素终点。
    /// @return 该带中心位置；n == 0 时返回 px0。
    [[nodiscard]] auto band_center_px(std::size_t i, float px0, float px1) const -> float {
        if (n_ == 0U) {
            return px0;
        }
        const std::size_t idx = std::min(i, n_ - 1U);
        const float step = (px1 - px0) / static_cast<float>(n_);
        return px0 + ((static_cast<float>(idx) + 0.5F) * step);
    }

    /// @brief 像素 → 类目索引（越界夹取，保证永远可安全下标）。
    /// @param px 待反查的像素坐标。
    /// @param px0 轴像素起点。
    /// @param px1 轴像素终点。
    /// @return 夹取到 [0, n-1] 的类目下标；n == 0 或带宽退化（≤ 1e-6）时返回 0。
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
/// @param index 系列索引（任意非负值，内部对 8 取模）。
/// @return 模 8 后对应位置的 RGBA 颜色。
[[nodiscard]] inline auto chart_palette(std::size_t index) -> Color {
    constexpr std::array<Color, 8> palette = {
        Color{66, 133, 244, 255},  // blue
        Color{219, 68, 55, 255},  // red
        Color{244, 180, 0, 255},  // yellow
        Color{15, 157, 88, 255},  // green
        Color{171, 71, 188, 255},  // purple
        Color{255, 112, 67, 255},  // deep orange
        Color{0, 172, 193, 255},  // cyan
        Color{124, 179, 66, 255},  // light green
    };
    // 取模后下标恒 < size()，越界分支不可达：at() 只是把「界内」写成可检查形式，不引入抛出路径。
    return palette.at(index % palette.size());
}

/// @brief 系列取色优先级：显式 color > `Theme` 命名令牌 `chart.palette.<i%8>` > 内置色板。
/// @param index 系列索引（令牌名与内置色板均按 index % 8 选取）。
/// @param explicit_color 系列显式色；有值时直接返回，不参与令牌查询。
/// @param theme 查询 `chart.palette.*` 命名令牌的主题。
/// @return 最终系列颜色（令牌缺失时回退内置色板）。
[[nodiscard]] inline auto resolve_series_color(std::size_t index, const std::optional<Color> &explicit_color,
                                               const Theme &theme) -> Color {
    if (explicit_color.has_value()) {
        return *explicit_color;
    }
    return theme.token_or<Color>("chart.palette." + std::to_string(index % 8U), chart_palette(index));
}

// ---------- JSON 编解码（数据进序列化面）----------

/// @brief `vector<double>` → JSON 数组（非数值元素跳过）。
/// @param v 数值序列。
/// @return JSON 数组；非有限值按 0.0 写出（保证 JSON 可序列化）。
[[nodiscard]] inline auto double_vector_to_json(const std::vector<double> &v) -> Json {
    Json a = Json::array();
    for (const double x : v) {
        a.push_back(std::isfinite(x) ? x : 0.0);
    }
    return a;
}

/// @brief JSON 数组 → `vector<double>`（非数组 / 非数值元素跳过，绝不抛异常）。
/// @param j 待解析的 JSON 值。
/// @return 数值序列（非数组时为空）；非有限数值按 0.0 收。
[[nodiscard]] inline auto json_to_double_vector(const Json &j) -> std::vector<double> {
    std::vector<double> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto &item : j) {
        if (item.is_number()) {
            const auto v = item.as_or<double>(0.0);
            out.push_back(std::isfinite(v) ? v : 0.0);
        }
    }
    return out;
}

/// @brief 字符串数组 ↔ JSON（类目标签）。
/// @param v 类目标签序列。
/// @return 与 v 等长的 JSON 字符串数组。
[[nodiscard]] inline auto string_vector_to_json(const std::vector<std::string> &v) -> Json {
    Json a = Json::array();
    for (const std::string &s : v) {
        a.push_back(s);
    }
    return a;
}

/// @brief JSON 数组 → 字符串数组（非数组 / 非字符串元素跳过，绝不抛异常）。
/// @param j 待解析的 JSON 值。
/// @return 字符串序列（非数组时为空）。
[[nodiscard]] inline auto json_to_string_vector(const Json &j) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto &item : j) {
        if (item.is_string()) {
            out.push_back(item.as_or<std::string>(""));
        }
    }
    return out;
}

/// @brief 单个系列 → JSON 对象（`color` 未设置时不输出，保留「按索引取色板」语义）。
/// @param s 待序列化的系列。
/// @return 含 `name` / `values`（可选 `color`）的 JSON 对象。
[[nodiscard]] inline auto chart_series_to_json(const ChartSeries &s) -> Json {
    Json o = Json::object();
    o.set("name", s.name);
    o.set("values", double_vector_to_json(s.values));
    if (s.color.has_value()) {
        o.set("color", color_to_json(*s.color));
    }
    return o;
}

/// @brief JSON 对象 → 系列（字段缺失 / 类型不符逐项回退默认，绝不抛异常）。
/// @param j 含 `name` / `values` / `color` 的 JSON 对象。
/// @return 解析出的系列（非对象时返回默认构造：空名、空值、无色）。
[[nodiscard]] inline auto json_to_chart_series(const Json &j) -> ChartSeries {
    ChartSeries s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j.at("name")->is_string()) {
        s.name = j.at("name")->as_or<std::string>("");
    }
    if (j.contains("values")) {
        s.values = json_to_double_vector(*j.at("values"));
    }
    if (j.contains("color") && j.at("color")->is_array()) {
        s.color = json_to_color(*j.at("color"));
    }
    return s;
}

/// @brief 对象数组（`ChartSeries`）↔ JSON。
/// @param v 系列序列。
/// @return 与 v 等长的 JSON 数组，逐元素走 `chart_series_to_json`。
[[nodiscard]] inline auto chart_series_vector_to_json(const std::vector<ChartSeries> &v) -> Json {
    Json a = Json::array();
    for (const ChartSeries &s : v) {
        a.push_back(chart_series_to_json(s));
    }
    return a;
}

/// @brief JSON 值 → `ChartSeries` 数组（非数组返回空；仅解析其中的对象元素）。
/// @param j 系列对象数组的 JSON 值。
/// @return 系列序列，逐元素走 `json_to_chart_series`。
[[nodiscard]] inline auto json_to_chart_series_vector(const Json &j) -> std::vector<ChartSeries> {
    std::vector<ChartSeries> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_chart_series(item));
        }
    }
    return out;
}

/// @brief 散点系列 → JSON 对象（点列为 `[x, y]` 数组对；非有限坐标按 0.0 写出）。
/// @param s 待序列化的散点系列。
/// @return 含 `name` / `points` / `dot_radius`（可选 `color`）的 JSON 对象。
[[nodiscard]] inline auto scatter_series_to_json(const ScatterSeries &s) -> Json {
    Json o = Json::object();
    o.set("name", s.name);
    Json pts = Json::array();
    for (const ChartPoint &pt : s.points) {
        Json p = Json::array();
        p.push_back(std::isfinite(pt.x) ? pt.x : 0.0);
        p.push_back(std::isfinite(pt.y) ? pt.y : 0.0);
        pts.push_back(std::move(p));
    }
    o.set("points", std::move(pts));
    o.set("dot_radius", s.dot_radius);
    if (s.color.has_value()) {
        o.set("color", color_to_json(*s.color));
    }
    return o;
}

/// @brief JSON 对象 → 散点系列（字段缺失 / 类型不符逐项回退默认，绝不抛异常）。
/// @param j 含 `name` / `points` / `dot_radius` / `color` 的 JSON 对象；`points` 仅收录
///        长度 ≥ 2 且前两元素为数值的数组项。
/// @return 解析出的散点系列（非对象时返回默认构造）。
[[nodiscard]] inline auto json_to_scatter_series(const Json &j) -> ScatterSeries {
    ScatterSeries s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j.at("name")->is_string()) {
        s.name = j.at("name")->as_or<std::string>("");
    }
    if (j.contains("dot_radius") && j.at("dot_radius")->is_number()) {
        s.dot_radius = j.at("dot_radius")->as_or<float>(0.0F);
    }
    if (j.contains("color") && j.at("color")->is_array()) {
        s.color = json_to_color(*j.at("color"));
    }
    if (j.contains("points") && j.at("points")->is_array()) {
        const auto *points = j.at("points");
        for (const auto &point : *points) {
            if (point.is_array() && point.size() >= 2 && point.at(0)->is_number() && point.at(1)->is_number()) {
                s.points.push_back(
                    ChartPoint{.x = point.at(0)->as_or<double>(0.0), .y = point.at(1)->as_or<double>(0.0)});
            }
        }
    }
    return s;
}

/// @brief 散点系列序列 → JSON 数组（逐元素走 `scatter_series_to_json`）。
/// @param v 散点系列序列。
/// @return 与 v 等长的 JSON 数组。
[[nodiscard]] inline auto scatter_series_vector_to_json(const std::vector<ScatterSeries> &v) -> Json {
    Json a = Json::array();
    for (const ScatterSeries &s : v) {
        a.push_back(scatter_series_to_json(s));
    }
    return a;
}

/// @brief JSON 值 → `ScatterSeries` 数组（非数组返回空；仅解析其中的对象元素）。
/// @param j 散点系列对象数组的 JSON 值。
/// @return 散点系列序列，逐元素走 `json_to_scatter_series`。
[[nodiscard]] inline auto json_to_scatter_series_vector(const Json &j) -> std::vector<ScatterSeries> {
    std::vector<ScatterSeries> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_scatter_series(item));
        }
    }
    return out;
}

/// @brief 扇区 → JSON 对象（非有限 `value` 按 0.0 写出；`color` 未设置时不输出）。
/// @param s 待序列化的扇区。
/// @return 含 `name` / `value`（可选 `color`）的 JSON 对象。
[[nodiscard]] inline auto pie_section_to_json(const PieSection &s) -> Json {
    Json o = Json::object();
    o.set("name", s.name);
    o.set("value", std::isfinite(s.value) ? s.value : 0.0);
    if (s.color.has_value()) {
        o.set("color", color_to_json(*s.color));
    }
    return o;
}

/// @brief JSON 对象 → 扇区（字段缺失 / 类型不符逐项回退默认；非有限 `value` 按 0.0 收）。
/// @param j 含 `name` / `value` / `color` 的 JSON 对象。
/// @return 解析出的扇区（非对象时返回默认构造：空名、0 值、无色）。
[[nodiscard]] inline auto json_to_pie_section(const Json &j) -> PieSection {
    PieSection s;
    if (!j.is_object()) {
        return s;
    }
    if (j.contains("name") && j.at("name")->is_string()) {
        s.name = j.at("name")->as_or<std::string>("");
    }
    if (j.contains("value") && j.at("value")->is_number()) {
        const auto v = j.at("value")->as_or<double>(0.0);
        s.value = std::isfinite(v) ? v : 0.0;
    }
    if (j.contains("color") && j.at("color")->is_array()) {
        s.color = json_to_color(*j.at("color"));
    }
    return s;
}

/// @brief 扇区序列 → JSON 数组（逐元素走 `pie_section_to_json`）。
/// @param v 扇区序列。
/// @return 与 v 等长的 JSON 数组。
[[nodiscard]] inline auto pie_section_vector_to_json(const std::vector<PieSection> &v) -> Json {
    Json a = Json::array();
    for (const PieSection &s : v) {
        a.push_back(pie_section_to_json(s));
    }
    return a;
}

/// @brief JSON 值 → `PieSection` 数组（非数组返回空；仅解析其中的对象元素）。
/// @param j 扇区对象数组的 JSON 值。
/// @return 扇区序列，逐元素走 `json_to_pie_section`。
[[nodiscard]] inline auto json_to_pie_section_vector(const Json &j) -> std::vector<PieSection> {
    std::vector<PieSection> out;
    if (!j.is_array()) {
        return out;
    }
    out.reserve(j.size());
    for (const auto &item : j) {
        if (item.is_object()) {
            out.push_back(json_to_pie_section(item));
        }
    }
    return out;
}

// ---------- 枚举编解码 ----------

/// @brief LegendPosition → JSON 字符串。
/// @param v 图例位置枚举值。
/// @return "Top" / "Bottom" / "Right" 之一（超出枚举范围的值兜底为 "Top"）。
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
/// @param j 待解析的 JSON 值（仅字符串参与匹配）。
/// @return 匹配 "Bottom" / "Right" 的对应枚举值；其余（含非字符串）一律为 Top。
[[nodiscard]] inline auto json_to_legend_position(const Json &j) -> LegendPosition {
    if (j.is_string()) {
        const auto s = j.as_or<std::string>("");
        if (s == "Bottom") {
            return LegendPosition::Bottom;
        }
        if (s == "Right") {
            return LegendPosition::Right;
        }
    }
    return LegendPosition::Top;
}

/// @brief 轴规格 → JSON（嵌套对象；空 `label` 与未设置的 `min`/`max` 不输出）。
/// @param a 待序列化的轴规格。
/// @return 含 visible / tick_count / show_grid_lines / include_zero 等字段的 JSON 对象。
[[nodiscard]] inline auto chart_axis_spec_to_json(const ChartAxisSpec &a) -> Json {
    Json o = Json::object();
    o.set("visible", Json{a.visible});
    if (!a.label.empty()) {
        o.set("label", a.label);
    }
    o.set("tick_count", a.tick_count);
    if (a.min.has_value()) {
        o.set("min", *a.min);
    }
    if (a.max.has_value()) {
        o.set("max", *a.max);
    }
    o.set("show_grid_lines", Json{a.show_grid_lines});
    o.set("include_zero", Json{a.include_zero});
    return o;
}

/// @brief JSON 对象 → 轴规格（字段缺失 / 类型不符逐项回退默认；`tick_count` 最小夹到 2）。
/// @param j 轴规格的 JSON 对象。
/// @return 解析出的轴规格（非对象时返回默认构造）。
[[nodiscard]] inline auto json_to_chart_axis_spec(const Json &j) -> ChartAxisSpec {
    ChartAxisSpec a;
    if (!j.is_object()) {
        return a;
    }
    if (j.contains("visible") && j.at("visible")->is_bool()) {
        a.visible = j.at("visible")->as_or<bool>(false);
    }
    if (j.contains("label") && j.at("label")->is_string()) {
        a.label = j.at("label")->as_or<std::string>("");
    }
    if (j.contains("tick_count") && j.at("tick_count")->is_number()) {
        a.tick_count = std::max(2, j.at("tick_count")->as_or<std::int32_t>(0));
    }
    if (j.contains("min") && j.at("min")->is_number()) {
        a.min = j.at("min")->as_or<double>(0.0);
    }
    if (j.contains("max") && j.at("max")->is_number()) {
        a.max = j.at("max")->as_or<double>(0.0);
    }
    if (j.contains("show_grid_lines") && j.at("show_grid_lines")->is_bool()) {
        a.show_grid_lines = j.at("show_grid_lines")->as_or<bool>(false);
    }
    if (j.contains("include_zero") && j.at("include_zero")->is_bool()) {
        a.include_zero = j.at("include_zero")->as_or<bool>(false);
    }
    return a;
}

/// @brief 图例规格 → JSON。
/// @param l 待序列化的图例规格。
/// @return 含 `visible` 与 `position`（按名序列化）的 JSON 对象。
[[nodiscard]] inline auto chart_legend_spec_to_json(const ChartLegendSpec &l) -> Json {
    Json o = Json::object();
    o.set("visible", Json{l.visible});
    o.set("position", legend_position_to_json(l.position));
    return o;
}

/// @brief JSON 对象 → 图例规格（字段缺失 / 类型不符逐项回退默认；position 未知值回退 Top）。
/// @param j 图例规格的 JSON 对象。
/// @return 解析出的图例规格（非对象时返回默认构造：可见、顶部）。
[[nodiscard]] inline auto json_to_chart_legend_spec(const Json &j) -> ChartLegendSpec {
    ChartLegendSpec l;
    if (!j.is_object()) {
        return l;
    }
    if (j.contains("visible") && j.at("visible")->is_bool()) {
        l.visible = j.at("visible")->as_or<bool>(false);
    }
    if (j.contains("position")) {
        l.position = json_to_legend_position(*j.at("position"));
    }
    return l;
}

// ---------- grow-in 动画载荷 ----------

/// @brief 图表入场动画（grow-in）：控制器 + 进度 `State<double>` + 帧循环注册/注销。
///
/// 进度写入 `State<double>`，随 `collect_signals` 参与响应式刷新，故动画不另起通道。
/// **无运行中 `Animator`（`Animator::current() == nullptr`，典型为 `render_to_png` / golden）时
/// 进度恒为 1**——终态降级，保证无头渲染输出确定。`reduce_motion` 由
/// `AnimationController::tick` 统一短路，本类无需特判。
///
/// 用法：控件持有一个成员，`on_mount` 调 `mount()`，数据变更调 `replay()`，绘制读 `progress()`。
class ChartGrowIn {
  public:
    /// @brief 默认构造：进度处于终态 1.0，尚未接入 `Animator`。
    ChartGrowIn() = default;
    /// @brief 析构：若控制器仍登记在 `Animator` 上则先摘除，避免帧循环持有悬空控制器。
    ~ChartGrowIn() {
        if (bound_) {
            if (Animator *a = Animator::current()) {
                a->remove(ctrl_);
            }
            bound_ = false;
        }
    }
    /// @brief 禁止拷贝：`Animator` 按控制器地址登记，拷贝会产生两份同源登记。
    ChartGrowIn(const ChartGrowIn &) = delete;
    /// @brief 禁止拷贝赋值（理由同拷贝构造）。
    /// @return 名义上返回左操作数引用；重载已删除，任何调用都是编译错误。
    auto operator=(const ChartGrowIn &) -> ChartGrowIn & = delete;
    /// @brief 禁止移动赋值：控件挂载后地址被 `Animator` 持有，赋值会撕裂登记。
    /// @return 名义上返回左操作数引用；重载已删除，任何调用都是编译错误。
    auto operator=(ChartGrowIn &&) -> ChartGrowIn & = delete;

    // 豁免 bugprone-exception-escape：本构造读写 State<double>（进订阅者通知链，回调经
    // std::function 转发），触发 .clang-tidy 已记录的系统性假告警面——「任何转入 std::function
    // 的可调用对象一律判『不应抛出』」（operator() 无 noexcept 规格）。抛出仅可能为 bad_alloc，
    // 由顶层兜底；noexcept 是既定契约（控件经 Node{widget} 入树，容器搬移依赖移动不抛），不改签名。
    /// @brief 移动构造：控件经 `Node{widget}` 入树必须可移动。
    ///
    /// 绑定建立在 `on_mount`（入树之后，此后不再移动），故此处若已登记则先摘除再转移进度值——
    /// 绝不让 `Animator` 持有已失效的控制器 / 目标地址（`Animator::drive` 存裸指针，UAF 风险）。
    /// @param other 源对象；其 `Animator` 登记被摘除，构造后进度回落终态 1.0。
    /// NOLINTNEXTLINE(bugprone-exception-escape)
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
    /// @return 归一化进度值。
    [[nodiscard]] auto progress() const -> double { return progress_.get(); }
    /// @brief 进度信号（供 `collect_signals` 登记）。
    /// @return `progress_` 的可变引用。
    [[nodiscard]] auto signal() -> State<double> & { return progress_; }
    /// @brief 是否正在播放（用于 `can_cache_display_list()`）。
    /// @return true = 已绑定帧循环且动画进行中。
    [[nodiscard]] auto animating() const -> bool { return bound_ && ctrl_.is_animating(); }

    /// @brief 在 `on_mount` 中调用：接上帧循环并从头播放；无 Animator 时落到终态。
    auto mount() -> void {
        Animator *a = Animator::current();  // 当前帧循环注册器；空 = 无头渲染上下文（降级路径）。
        if (a == nullptr) {
            progress_.set(1.0);  // 降级：无运行循环（无头渲染）直接呈现终态。
            return;
        }
        progress_.set(0.0);  // 从头播放：绑定帧循环前将进度复位到 0。
        a->bind(ctrl_, Tween<double>{0.0, 1.0, Curves::ease_out()}, progress_);
        bound_ = true;
        ctrl_.forward();  // 起播：0 → 1 缓出过渡（450ms）。
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
    State<double> progress_{1.0};  ///< 初值为终态（未播放 = 已完成）
    bool bound_ = false;  ///< 是否已登记进 Animator（决定析构是否摘除）
};

// ---------- 轴绘制共享实现（Bar / Line / Scatter 共用，避免四份漂移）----------

/// @brief 刻度小数位：由 nice step 推导（step ≥ 1 → 0 位）。
/// @param step 比例尺的 nice 步长（非正 / 非有限按 0 位处理）。
/// @return 保留小数位数，夹取在 [0, 6]。
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
/// @param p 目标绘制器。
/// @param origin 控件全局原点（绘制时平移量）。
/// @param plot 局部坐标绘图区（高度 ≤ 0 时直接返回，不绘制）。
/// @param scale 数值域比例尺（提供 ticks 与 to_px 换算）。
/// @param spec 轴规格（`show_grid_lines` 控制网格线，`label` 非空时绘制轴标题）。
/// @param font 刻度与标题字体。
/// @param grid_color 网格线颜色。
/// @param text_color 刻度与轴标题文字颜色。
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
        const Rect box{
            .origin = Point{.x = origin.x + plot.origin.x - w - 6.0F - line_h, .y = origin.y + plot.origin.y},
            .size = Size{.width = w + 2.0F, .height = line_h}};
        p.draw_text(box, spec.label, font, text_color);
    }
}

/// @brief 类目轴（x）绘制：带中心标签 + 轴标题。`cats` 为空时直接返回。
/// @param p 目标绘制器。
/// @param origin 控件全局原点（绘制时平移量）。
/// @param plot 局部坐标绘图区（宽度 ≤ 0 时直接返回，不绘制）。
/// @param band 类目比例尺（提供各带中心像素）。
/// @param cats 类目标签序列（逐项画在对应带中心下方）。
/// @param spec 轴规格（`label` 非空时在绘图区下方居中绘制轴标题）。
/// @param font 标签与标题字体。
/// @param text_color 标签与轴标题文字颜色。
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

/// @brief 扇区归一化占比（Σ ≤ 0 时返回全 0，由调用方按降级口径处理）。
/// @param sections 扇区序列（仅有限且 > 0 的 `value` 计入总和）。
/// @return 与 sections 等长的占比序列（每项 = value / Σvalue；无效/非正项为 0）。
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
