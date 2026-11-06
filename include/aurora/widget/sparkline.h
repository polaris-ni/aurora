#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/core/json.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/chart_common.h"
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief Sparkline 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 六行皆为**可空值件 + 常量档回落点**：迷你走势图没有随作用域而变的属性（取色走内置色板而非主题
/// 扩展），故六枚读件一律无参；「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前 `values` / `line_width` / `show_end_dot` / `dot_radius` / `padding` 是带声明处默认值的裸
/// 字段、`color` 是 `std::optional`；现在六行统一为「可空 ⇔ 非必填 + 行上回落点」，声明处默认值
/// 逐个落成该行的回落基线（`values` 的基线就是空序列），生效值逐值不变。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class SparklineProps : public AbstractProps<SparklineProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：单系列数据（等距，x = 索引）。改变数据只影响绘制；回落点是空序列。
    ///
    /// 本行的裸值是 `std::vector<double>`，其编解码登记见 `props.h` 的 `PropCodec<std::vector<double>>`：
    /// 数组元素按既有口径**逐项宽容**（非数组退空序列、非数值元素跳过），故未登记 `validate`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto values_row() {
        return prop_row<&SparklineProps::values_>("values")
            .paint()
            .fallback(&SparklineProps::values_default)
            .note("Single series data (evenly spaced)");
    }

    /// @brief 可空值件：线条色。回落点是内置色板的第 0 号色（不读注册的 `ChartPalette` 扩展）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto color_row() {
        return prop_row<&SparklineProps::color_>("color")
            .paint()
            .fallback(&SparklineProps::color_default)
            .shows("palette[0]")
            .note("Line color; defaults to the first color of the built-in palette");
    }

    /// @brief 可空值件：线宽（dp）；回落基线 1.5，下界 0（0 是「显式要一条不画出来的线」）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto line_width_row() {
        return prop_row<&SparklineProps::line_width_>("line_width")
            .paint()
            .bounds("0")
            .fallback(&SparklineProps::line_width_default)
            .note("Line width (dp)");
    }

    /// @brief 可空值件：末端数据点圆点开关；回落基线为开。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_end_dot_row() {
        return prop_row<&SparklineProps::show_end_dot_>("show_end_dot")
            .paint()
            .fallback(&SparklineProps::show_end_dot_default)
            .note("Dot on the last data point");
    }

    /// @brief 可空值件：末端圆点半径（dp）；回落基线 2.0，下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto dot_radius_row() {
        return prop_row<&SparklineProps::dot_radius_>("dot_radius")
            .paint()
            .bounds("0")
            .fallback(&SparklineProps::dot_radius_default)
            .note("End dot radius (dp)");
    }

    /// @brief 可空值件：图内留白（dp）。只内缩绘图矩形、不改控件尺寸 ⇒ 绘制档；回落基线四边各 2.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&SparklineProps::padding_>("padding")
            .paint()
            .fallback(&SparklineProps::padding_default)
            .note("In-chart padding (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(values_row(), color_row(), line_width_row(), show_end_dot_row(), dot_radius_row(),
                              padding_row());
    }

    // ---- 读面：每行恰一件，六行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的数据序列。
    /// @return 生效序列（数组行按值给出：未表态时为空序列）。
    [[nodiscard]] auto values() const -> std::vector<double> { return stated(values_row()); }

    /// @brief 当前生效的线条色。
    /// @return 生效线色；未表态时为内置色板第 0 号。
    [[nodiscard]] auto color() const -> Color { return stated(color_row()); }

    /// @brief 当前生效的线宽（dp）。
    /// @return 生效线宽。
    [[nodiscard]] auto line_width() const -> float { return stated(line_width_row()); }

    /// @brief 当前生效的末端圆点开关。
    /// @return 生效开关。
    [[nodiscard]] auto show_end_dot() const -> bool { return stated(show_end_dot_row()); }

    /// @brief 当前生效的末端圆点半径（dp）。
    /// @return 生效半径。
    [[nodiscard]] auto dot_radius() const -> float { return stated(dot_radius_row()); }

    /// @brief 当前生效的图内留白（dp）。
    /// @return 生效留白。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入数据序列；表态即脱离空序列基线。
    /// @param value 新序列（等距，x = 索引）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_values(const std::vector<double> &value) -> SparklineProps & { return assign(values_row(), value); }

    /// @brief 装入线条色；表态即脱离内置色板第 0 号。
    /// @param value 新线色。
    /// @return 引用本属性对象，便于链式续写。
    auto set_color(Color value) -> SparklineProps & { return assign(color_row(), value); }

    /// @brief 装入线宽（dp）。
    /// @param value 新线宽；0 是「显式要一条不画出来的线」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_line_width(float value) -> SparklineProps & { return assign(line_width_row(), value); }

    /// @brief 装入末端圆点开关。
    /// @param value true = 绘制末端圆点。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_end_dot(bool value) -> SparklineProps & { return assign(show_end_dot_row(), value); }

    /// @brief 装入末端圆点半径（dp）。
    /// @param value 新半径。
    /// @return 引用本属性对象，便于链式续写。
    auto set_dot_radius(float value) -> SparklineProps & { return assign(dot_radius_row(), value); }

    /// @brief 装入图内留白（dp）。
    /// @param value 四边留白。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> SparklineProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 数据序列回落基线。
    /// @return 空序列（无数据 ⇒ 不画）。
    [[nodiscard]] static auto values_default() -> std::vector<double> { return {}; }

    /// @brief 线条色回落基线。
    /// @return 内置色板第 0 号色（与主题注册的 `ChartPalette` 扩展无关）。
    [[nodiscard]] static auto color_default() -> Color { return chart_palette(0U); }

    /// @brief 线宽回落基线。
    /// @return 1.5F（dp）。
    [[nodiscard]] static auto line_width_default() -> float { return 1.5F; }

    /// @brief 末端圆点开关回落基线。
    /// @return true（默认画末端圆点）。
    [[nodiscard]] static auto show_end_dot_default() -> bool { return true; }

    /// @brief 末端圆点半径回落基线。
    /// @return 2.0F（dp）。
    [[nodiscard]] static auto dot_radius_default() -> float { return 2.0F; }

    /// @brief 图内留白回落基线。
    /// @return 四边各 2.0dp。
    [[nodiscard]] static auto padding_default() -> EdgeInsets {
        return EdgeInsets{.left = 2.0F, .top = 2.0F, .right = 2.0F, .bottom = 2.0F};
    }

  private:
    std::optional<std::vector<double>> values_;  ///< 数据序列表态位
    std::optional<Color> color_;  ///< 线色表态位
    std::optional<float> line_width_;  ///< 线宽表态位（dp）
    std::optional<bool> show_end_dot_;  ///< 末端圆点开关表态位
    std::optional<float> dot_radius_;  ///< 末端圆点半径表态位（dp）
    std::optional<EdgeInsets> padding_;  ///< 图内留白表态位（dp）

    friend class AbstractProps<SparklineProps>;
};

/// @brief 迷你折线（叶控件；契约见 specification/04-widget.md §3.8）。
///
/// **无轴、无网格、无图例、无交互**——最薄的图表控件，用于表格 / 卡片内的趋势缩览。
/// 值域直接取数据 min/max（退化时回退 `[0,1]`），绘制 = `stroke_polyline` + 末端圆点。
///
/// 属性一律经 `SparklineProps` 的读写件访问（本类公有继承它，故 `spark.set_line_width(2.0F)` 与
/// `spark.line_width()` 都直接可用）；六行皆常量档，读件一律无参。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Sparkline : public LeafWidget, public SparklineProps {
  public:
    /// @brief 默认构造：六行皆未表态（生效值即各行回落基线）；并接线宿主。
    Sparkline() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 迷你折线属性对象，移动进本实例的 `SparklineProps` 基类子对象。
    explicit Sparkline(SparklineProps props) : SparklineProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `SparklineProps`：各行未表态，生效值即该行回落点
    [[nodiscard]] static auto defaults() -> SparklineProps { return {}; }

    /// @brief 装入数据序列：转调属性面写件，再重播 grow-in 入场动画。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的写件：动画重播是控件自身的事实，不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值仍由同一行存储与回落给出）。
    /// @param v 新序列（等距，x = 索引）。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_values(const std::vector<double> &v) -> Sparkline & {
        SparklineProps::set_values(v);
        grow_.replay();  // 重播 grow-in 入场动画
        return *this;
    }

    /// @brief 类型名字符串 "Sparkline"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override { return "Sparkline"; }

    /// @brief 静态属性描述符（供注册表 / Schema 生成使用）。
    /// @return 含权威表六行与基类属性的描述表：`properties` 按 `rows()` 点名顺序，无事件、无子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Sparkline");
        descriptor.children_policy = "none";
        descriptor.invariants = {"line_width >= 0"};
        descriptor.examples = {"au::Sparkline(au::SparklineProps().set_values({4.0, 6.0, 3.0, 8.0, 5.0}))"};
        return descriptor;
    }

    /// @brief 运行时自描述：转发到静态描述符。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 登记需订阅的信号视图：本表六行皆为值形态，可登记的只有 grow-in 动画的进行信号。
    /// @param out 信号采集出口；追加动画进行信号的裸指针。
    auto collect_signals(StateSink &out) -> void override {
        collect_prop_sources(out);
        out.push(grow_.signal());
    }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    /// @return grow-in 入场动画未进行中时为 true（本帧绘制结果可缓存复用），动画播放期间为 false。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    /// @brief 序列化自有属性到 `props`：先由基类写通用属性，再写各行；未表态的行不写键。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 已登记校验的行（线宽 / 半径越界、颜色与留白形态不符）非法值一律不写入并撤表态；
    ///       数据数组按既有宽容口径逐项跳过（不登记校验，故照常表态且不上报）。
    /// @param props 源 JSON 对象，按权威表逐行取键。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 无障碍角色：图表族统一为 `Image`—— 推断表不识 `Sparkline`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @note Side-effects: pure
    /// @return 固定为 `AccessibilityRole::Image`。
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    /// @brief 无障碍标签：类型名 + 数据点数量。
    /// @return 形如 "Sparkline, N points" 的字符串。
    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值：末端数据点读数。
    /// @return 最后一个值的字符串形式；数据为空时为空串。
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

    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态）。
    auto on_mount(const BuildContext & /*ctx*/) -> void override { grow_.mount(); }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 数组行按值读出，故本帧只读一次再复用；其余五行同为一次性读出，逐档不参与取模。
        const std::vector<double> data = values();
        const EdgeInsets pad = padding();
        const float stroke = line_width();
        const Color line = color();
        const bool end_dot = show_end_dot();
        const float radius = dot_radius();
        const float left = bounds.origin.x + pad.left;
        const float top = bounds.origin.y + pad.top;
        const float w = std::max(0.0F, bounds.size.width - pad.left - pad.right);
        const float h = std::max(0.0F, bounds.size.height - pad.top - pad.bottom);
        if (w <= 0.0F || h <= 0.0F || data.size() < 2 || stroke <= 0.0F) {
            return;
        }
        double lo = 0.0;
        double hi = 0.0;
        bool have = false;
        for (const double v : data) {
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
        // 主题在本控件无消费点（sparkline 取内置 `chart_palette`，不读注册的 `ChartPalette` 扩展，
        // 六行又皆为常量档）：此行只作 `ctx` 的显式消费位保留，按引用读取故零拷贝。
        const Theme &theme = inherit_theme(ctx);
        std::vector<Point> pts;
        pts.reserve(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            const double v = std::isfinite(data[i]) ? data[i] : lo;
            const float t = static_cast<float>(i) / static_cast<float>(data.size() - 1U);
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
        p.stroke_polyline(drawn, stroke, line);
        if (end_dot && radius > 0.0F && grow_.progress() >= 1.0) {
            const Point &last = pts.back();
            const float d = radius * 2.0F;
            // 末端圆点须夹在控件内（越界像素不会被脏区擦除）
            const float cx = std::clamp(last.x, bounds.origin.x + radius, bounds.origin.x + bounds.size.width - radius);
            const float cy =
                std::clamp(last.y, bounds.origin.y + radius, bounds.origin.y + bounds.size.height - radius);
            p.fill_rounded_rect(
                Rect{.origin = Point{.x = cx - radius, .y = cy - radius}, .size = Size{.width = d, .height = d}},
                radius, line);
        }
        (void)theme;
    }

  private:
    ChartGrowIn grow_;  ///< grow-in 入场动画（进行信号驱动逐帧重绘，播放期不缓存 Display List）
};

// N 取 values().size()，非有限值也计入。
inline auto Sparkline::accessibility_label() const -> std::string {
    return "Sparkline, " + std::to_string(values().size()) + " points";
}

// 直接取末值，非有限值照原样转字符串。
inline auto Sparkline::accessibility_value() const -> std::string {
    const std::vector<double> data = values();
    if (data.empty()) {
        return std::string{};
    }
    return std::to_string(data.back());
}

}  // namespace aurora
