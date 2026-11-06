#pragma once

#include <algorithm>
#include <cmath>
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
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief ScatterChart 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 六行皆为**可空值件 + 常量档回落点**：散点图的配色取自继承主题与色板、点半径住在 `ScatterSeries`
/// 结构体里而非属性面，故六枚读件一律无参；「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前六行都是带声明处默认值的裸字段；现在声明处默认值逐个落成各行回落基线（`series` 的基线是空序列，
/// `axis_x` / `axis_y` / `legend` 的基线是各自默认构造），生效值逐值不变。
///
/// 档位按「哪几趟消费本行」给出，与迁移前逐枚 setter 手写的标脏口径有五行不同，其中三行是行为修正：
/// `axis_x` / `axis_y` / `legend` 迁移前只标布局，但三者都被**绘制趟**直接读（轴的可见性与标签和网格线开关
/// 决定刻度画不画，图例可见性与名称决定色块与文字），单标布局档会让绘制趟停在旧值。另两行只是口径归位：
/// `padding` 迁移前只标布局，此处随图表族同键口径补绘制档（本型绘制趟确实直接读 `axis_*` 与 `legend`，
/// 留白则经由缓存的几何参与）；`show_crosshair` 迁移前**没有写件**（只能经字段直写或 JSON 抵达），本表按
/// 「每行恰一写件」给出写件并只标绘制档。`series` 迁移前后同档（布局 + 绘制），它的动画重播仍留在控件侧
/// 那枚同名覆写里。
///
/// 两组形态走的是两条不同的校验通路。`series` 与 `axis_x` / `axis_y` / `legend` 四行的编解码登记在
/// `chart_common.h` 里：类型名、JSON 类型与双向转换都由登记给出，本表只点名它们，而它们一律**未登记
/// `validate`** —— 迁移前那几条通路就是「字段缺失 / 类型不符逐项回退默认」的宽容口径，故这类行非法值
/// 不撤销表态、也不产生降级上报（`points` 里长度不足或非数值的数组项直接不收录，同样不撤整行表态）。
/// 两枚标量行 `show_crosshair` / `padding` 则相反：`bool` 与 `EdgeInsets` 登记自带校验，故旧路径的
/// 「非 bool 静默跳过、保留现值」与「`json_to_edge_insets` 直接覆盖（非对象整值烘成四边全 0 且照常取值）」
/// 都不复存在——形态不符一律拒写、撤表态并上报降级。运行时写件里 `show_crosshair` 与迁移前同（本来就没有
/// 写件），其余五行的写件只是换了入口，存进去的值与读回来的值逐一相同。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class ScatterChartProps : public AbstractProps<ScatterChartProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：散点系列（各含显式 `ChartPoint` 坐标）；回落点是空序列。
    ///
    /// 档位取「布局 + 绘制」：域计算、几何预留与画点三处都读它，不只是画。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto series_row() {
        return prop_row<&ScatterChartProps::series_>("series")
            .layout()
            .paint()
            .fallback(&ScatterChartProps::series_default)
            .note("Scatter series array: [{name, points:[[x,y],...], color?, dot_radius}]");
    }

    /// @brief 可空值件：悬停十字准线开关（吸附最近点）；回落基线为开。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_crosshair_row() {
        return prop_row<&ScatterChartProps::show_crosshair_>("show_crosshair")
            .paint()
            .fallback(&ScatterChartProps::show_crosshair_default)
            .note("Draw a crosshair snapped to the nearest point on hover");
    }

    /// @brief 可空值件：x 轴（Linear）规格。回落点是默认构造的 `ChartAxisSpec`（可见、五刻度、画网格）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_x_row() {
        return prop_row<&ScatterChartProps::axis_x_>("axis_x")
            .layout()
            .paint()
            .fallback(&ScatterChartProps::axis_x_default)
            .note("x axis spec: {visible,label,tick_count,min,max,show_grid_lines}");
    }

    /// @brief 可空值件：y 轴（Linear）规格。回落点是默认构造的 `ChartAxisSpec`（无显式域 ⇒ 域按数据 nice 化）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_y_row() {
        return prop_row<&ScatterChartProps::axis_y_>("axis_y")
            .layout()
            .paint()
            .fallback(&ScatterChartProps::axis_y_default)
            .note("y axis spec: {visible,label,tick_count,min,max,show_grid_lines}");
    }

    /// @brief 可空值件：图例规格。回落点是默认构造的 `ChartLegendSpec`（可见、顶部）。
    ///
    /// 本行同时进布局（图例带扣减绘图区）与绘制（色块与名称），故两档齐标。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto legend_row() {
        return prop_row<&ScatterChartProps::legend_>("legend")
            .layout()
            .paint()
            .fallback(&ScatterChartProps::legend_default)
            .note("Legend spec: {visible,position:Top|Bottom|Right}");
    }

    /// @brief 可空值件：图内留白（dp），轴标签 / 图例带的避让区。回落基线四边各 8.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&ScatterChartProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&ScatterChartProps::padding_default)
            .note("In-chart padding (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(series_row(), show_crosshair_row(), axis_x_row(), axis_y_row(), legend_row(),
                              padding_row());
    }

    // ---- 读面：每行恰一件，六行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的散点系列。
    /// @return 生效序列（数组行按值给出：未表态时为空序列）。
    [[nodiscard]] auto series() const -> std::vector<ScatterSeries> { return stated(series_row()); }

    /// @brief 当前生效的十字准线开关。
    /// @return 生效开关。
    [[nodiscard]] auto show_crosshair() const -> bool { return stated(show_crosshair_row()); }

    /// @brief 当前生效的 x 轴规格。
    /// @return 生效规格（未表态时为默认构造）。
    [[nodiscard]] auto axis_x() const -> ChartAxisSpec { return stated(axis_x_row()); }

    /// @brief 当前生效的 y 轴规格。
    /// @return 生效规格（未表态时为默认构造）。
    [[nodiscard]] auto axis_y() const -> ChartAxisSpec { return stated(axis_y_row()); }

    /// @brief 当前生效的图例规格。
    /// @return 生效规格（未表态时为可见、顶部）。
    [[nodiscard]] auto legend() const -> ChartLegendSpec { return stated(legend_row()); }

    /// @brief 当前生效的图内留白（dp）。
    /// @return 生效留白。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入散点系列；表态即脱离空序列基线。
    /// @param value 新系列列表（各含点名与显式坐标点集）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_series(const std::vector<ScatterSeries> &value) -> ScatterChartProps & {
        return assign(series_row(), value);
    }

    /// @brief 装入十字准线开关。
    /// @param value true 时悬停绘制吸附准线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_crosshair(bool value) -> ScatterChartProps & { return assign(show_crosshair_row(), value); }

    /// @brief 装入 x 轴规格（可见性 / 标签 / 刻度数 / 显式域 / 网格线）。
    /// @param value x 轴规格。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_x(const ChartAxisSpec &value) -> ScatterChartProps & { return assign(axis_x_row(), value); }

    /// @brief 装入 y 轴规格（可见性 / 标签 / 刻度数 / 显式域 / 网格线）。
    /// @param value y 轴规格。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_y(const ChartAxisSpec &value) -> ScatterChartProps & { return assign(axis_y_row(), value); }

    /// @brief 装入图例规格。
    /// @param value 图例规格（可见性 / 位置）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_legend(const ChartLegendSpec &value) -> ScatterChartProps & { return assign(legend_row(), value); }

    /// @brief 装入图内留白（dp）。
    /// @param value 四边留白。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> ScatterChartProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 散点系列回落基线。
    /// @return 空序列（无数据 ⇒ 不画）。
    [[nodiscard]] static auto series_default() -> std::vector<ScatterSeries> { return {}; }

    /// @brief 十字准线开关回落基线。
    /// @return true（默认画准线）。
    [[nodiscard]] static auto show_crosshair_default() -> bool { return true; }

    /// @brief x 轴回落基线。
    /// @return 默认构造的轴规格（可见、五刻度、画网格、域含 0）。
    [[nodiscard]] static auto axis_x_default() -> ChartAxisSpec { return {}; }

    /// @brief y 轴回落基线。
    /// @return 默认构造的轴规格（无显式域 ⇒ 域按数据 nice 化）。
    [[nodiscard]] static auto axis_y_default() -> ChartAxisSpec { return {}; }

    /// @brief 图例回落基线。
    /// @return 默认构造的图例规格（可见、顶部）。
    [[nodiscard]] static auto legend_default() -> ChartLegendSpec { return {}; }

    /// @brief 图内留白回落基线。
    /// @return 四边各 8.0dp。
    [[nodiscard]] static auto padding_default() -> EdgeInsets {
        return EdgeInsets{.left = 8.0F, .top = 8.0F, .right = 8.0F, .bottom = 8.0F};
    }

  private:
    std::optional<std::vector<ScatterSeries>> series_;  ///< 散点系列表态位
    std::optional<bool> show_crosshair_;  ///< 十字准线开关表态位
    std::optional<ChartAxisSpec> axis_x_;  ///< x 轴规格表态位
    std::optional<ChartAxisSpec> axis_y_;  ///< y 轴规格表态位
    std::optional<ChartLegendSpec> legend_;  ///< 图例规格表态位
    std::optional<EdgeInsets> padding_;  ///< 图内留白表态位（dp）

    friend class AbstractProps<ScatterChartProps>;
};

/// @brief 散点图控件（叶控件；契约见 specification/04-widget.md §3.8）。
///
/// x / y 双 `LinearScale`（域由数据推导，可经 `axis_*.min/max` 覆盖），圆点绘制。
/// 命中按**最近点欧氏距离**（阈值 = `dot_radius + 4dp`），与渲染同源于同一组比例尺。
///
/// 属性一律经 `ScatterChartProps` 的读写件访问（本类公有继承它，故 `chart.set_axis_x(spec)` 与
/// `chart.show_crosshair()` 都直接可用）；六行皆常量档，读件一律无参。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class ScatterChart : public LeafWidget, public ScatterChartProps {
  public:
    /// @brief 默认构造：六行皆未表态（生效值即各行回落基线）；并接线宿主。
    ScatterChart() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 散点图属性对象，移动进本实例的 `ScatterChartProps` 基类子对象。
    explicit ScatterChart(ScatterChartProps props) : ScatterChartProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `ScatterChartProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> ScatterChartProps { return {}; }

    /// @brief 点击（抬起）命中数据点时触发：`(系列索引, 点索引)`。旁挂，不进序列化面。
    /// @param series_idx 回调收到的系列索引（0 基）。
    /// @param point_idx 回调收到的点索引（系列内 0 基）。
    /// @return 回调函数对象；缺省未挂接（空 std::function）。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    /// @brief 装入散点系列：转调属性面写件，再重播 grow-in 入场动画。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的写件：动画重播是控件自身的事实，不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值仍由同一行存储与回落给出）。
    /// @param value 新系列列表（各含点名与显式坐标点集）。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_series(const std::vector<ScatterSeries> &value) -> ScatterChart & {
        ScatterChartProps::set_series(value);
        grow_.replay();  // 重播 grow-in 入场动画（未接帧循环时为 no-op）
        return *this;
    }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "ScatterChart"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "ScatterChart"; }

    /// @brief 静态自描述表：属性矩阵由权威表派生，事件 / 不变式 / 样例留在本处。
    /// @return WidgetDescriptor，含 width/height/show 公共属性与 on_point_tapped 事件。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（属性/事件/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的响应式信号到构建面。
    /// @param out 输出收集向量（追加各行已绑定的外部来源，以及 grow_ 动画进度信号的基类指针）。
    auto collect_signals(StateSink &out) -> void override {
        collect_prop_sources(out);
        out.push(grow_.signal());
    }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    /// @return grow_ 动画进行中为 false，可缓存时为 true。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    /// @brief 序列化自有属性到 `props`：先由基类写通用属性，再写各行；未表态的行不写键。
    /// @param props 输出目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 已登记校验的行（`show_crosshair` 非 bool / `padding` 非对象或字段非数值）非法值一律不写入并撤
    ///        表态；系列与双轴、图例按既有宽容口径逐项回退默认（不登记校验，故照常表态且不上报）。
    /// @param props 源 JSON 对象，按权威表逐行取键。
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

    /// @brief 无障碍角色：图表族统一为 `Image`—— 推断表不识 `ScatterChart`，
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
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态）。
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
    [[nodiscard]] static auto legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t>;
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

// 非有限坐标跳过；首个有限点确定 lo/hi 初值，之后逐项收窄。属性行按「每趟一份快照」读。
inline auto ScatterChart::domain_of(bool x_axis) const -> std::pair<double, double> {
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    const auto secs = series();
    for (const ScatterSeries &s : secs) {
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
    const ChartAxisSpec spec = x_axis ? axis_x() : axis_y();
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
    const ChartAxisSpec spec = x_axis ? axis_x() : axis_y();
    if (spec.min.has_value() && spec.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, spec.tick_count);
    }
    return LinearScale::from_domain(lo, hi, spec.tick_count);
}

// 绘图区依次扣减留白、图例占位与两轴的刻度/标签占位，再按图例位置逐项排布命中矩形。
// 属性行按「每趟一份快照」读：本趟开头一次取齐系列、留白、图例与双轴，避免同一趟里读到不一致的值。
inline auto ScatterChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    const auto secs = series();
    const auto pad = padding();
    const auto leg = legend();
    const ChartAxisSpec ax = axis_x();
    const ChartAxisSpec ay = axis_y();
    g.x_scale = scale_of(true);
    g.y_scale = scale_of(false);

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = pad.left;
    float top = pad.top;
    float right = std::max(pad.left, size.width - pad.right);
    float bottom = std::max(pad.top, size.height - pad.bottom);

    if (leg.visible && !secs.empty()) {
        if (leg.position == LegendPosition::Top) {
            top += line_h;
        } else if (leg.position == LegendPosition::Bottom) {
            bottom -= line_h;
        } else {
            float widest = 0.0F;
            for (const ScatterSeries &s : secs) {
                widest = std::max(widest, render::FontEngine::measure_width(s.name, font));
            }
            right = std::max(left, right - (widest + 22.0F));
        }
    }
    if (ax.visible) {
        bottom -= line_h;
        if (!ax.label.empty()) {
            bottom -= line_h;
        }
    }
    if (ay.visible) {
        float widest = 0.0F;
        const int digits = tick_digits(g.y_scale.step());
        for (const double t : g.y_scale.ticks()) {
            widest = std::max(widest, render::FontEngine::measure_width(format_number(t, Locale{}, digits), font));
        }
        left += widest + 6.0F;
        if (!ay.label.empty()) {
            left += line_h;
        }
    }
    g.plot = Rect{.origin = Point{.x = left, .y = top},
                  .size = Size{.width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)}};

    if (leg.visible && !secs.empty()) {
        float cursor_x = g.plot.origin.x;
        // Right（含未知位置）沿用绘图区顶部，即下面 else 分支不再重复赋同一值——
        // 重复赋值会让本初值在三条互斥分支下都永不被读（死存储）。
        float cursor_y = g.plot.origin.y;
        if (leg.position == LegendPosition::Top) {
            cursor_y = pad.top;
        } else if (leg.position == LegendPosition::Bottom) {
            cursor_y = size.height - pad.bottom - line_h;
        } else {
            cursor_x = g.plot.right() + 8.0F;
        }
        for (const ScatterSeries &s : secs) {
            const float name_w = render::FontEngine::measure_width(s.name, font);
            const float item_w = 14.0F + name_w;
            if (leg.position != LegendPosition::Right && cursor_x + item_w > size.width - pad.right) {
                break;
            }
            g.legend_rects.push_back(
                Rect{.origin = Point{.x = cursor_x, .y = cursor_y}, .size = Size{.width = item_w, .height = line_h}});
            if (leg.position == LegendPosition::Right) {
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
    geom_ = compute_geometry(s, inherit_theme(ctx).type_scale.body_medium);
    return s;
}

// 顺序扫描 legend_rects，首个包含该点的矩形即命中。
inline auto ScatterChart::legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

// 非有限坐标的点跳过；阈值按所属系列的 dot_radius 计，逐项与当前最近者比较。系列每趟取一份快照。
inline auto ScatterChart::nearest_point(const Geometry &g, const Point &local) const
    -> std::optional<std::pair<int, int>> {
    const auto secs = series();
    if (g.plot.size.width <= 0.0F || g.plot.size.height <= 0.0F || secs.empty()) {
        return std::nullopt;
    }
    float best = std::numeric_limits<float>::max();
    std::optional<std::pair<int, int>> best_hit;
    for (std::size_t k = 0; k < secs.size(); ++k) {
        const float threshold = secs[k].dot_radius + 4.0F;
        for (std::size_t i = 0; i < secs[k].points.size(); ++i) {
            const ChartPoint &pt = secs[k].points[i];
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

// 总点数为各系列 points.size() 之和（系列每趟取一份快照）。
inline auto ScatterChart::accessibility_label() const -> std::string {
    const auto secs = series();
    std::size_t total = 0;
    for (const ScatterSeries &s : secs) {
        total += s.points.size();
    }
    return "ScatterChart, " + std::to_string(secs.size()) + " series, " + std::to_string(total) + " points";
}

// 索引由悬停事件写入，读回时仍校验负值与越界；值取生效系列（每趟一份快照）。
inline auto ScatterChart::accessibility_value() const -> std::string {
    const auto secs = series();
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const auto [k, i] = *hovered_point_;
    if (k < 0 || i < 0 || static_cast<std::size_t>(k) >= secs.size() ||
        static_cast<std::size_t>(i) >= secs[static_cast<std::size_t>(k)].points.size()) {
        return std::string{};
    }
    const ChartPoint &pt = secs[static_cast<std::size_t>(k)].points[static_cast<std::size_t>(i)];
    return secs[static_cast<std::size_t>(k)].name + ": (" + std::to_string(pt.x) + ", " + std::to_string(pt.y) + ")";
}

// 属性矩阵由权威表派生（键名、类型、默认值与说明逐行取自各行登记），事件 / 不变式 / 样例留在本处。
inline auto ScatterChart::describe_static() -> WidgetDescriptor {
    auto descriptor = props_descriptor("ScatterChart");
    descriptor.events = {"on_point_tapped"};
    descriptor.children_policy = "none";
    descriptor.invariants = {"dot_radius >= 0"};
    descriptor.examples = {
        "au::ScatterChart(au::ScatterChartProps().set_series({{ .name = \"A\", .points = "
        "{{1, 2}, {3, 4}} }}))"};
    return descriptor;
}

// 逐行序列化：只写已表态的行，未表态的键不落盘（落盘的是意图，不是现值）。
inline auto ScatterChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props_serialize(props);
}

// 逐行反序列化：已登记校验的两行形态不符即不写入并撤表态，系列与双轴、图例按宽容口径逐项回退默认。
inline auto ScatterChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    props_deserialize(props);
}

// 绘制顺序：y 轴与网格 → x 轴数值刻度 → 数据点 → 图例 → 悬停准线/圆环/值框。
// 属性行按「每趟一份快照」读：双轴、系列、图例与准线开关各取一次，与 compute_geometry 同口径。
inline auto ScatterChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme &theme = inherit_theme(ctx);
    const Font &font = theme.type_scale.body_medium;
    const Point origin = bounds.origin;
    const Geometry &g = geom_;
    const Rect &plot = g.plot;
    const auto secs = series();
    const ChartAxisSpec ax = axis_x();
    const ChartAxisSpec ay = axis_y();
    const auto leg = legend();
    const bool crosshair = show_crosshair();
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 31};
    const Color axis = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 160};

    if (ay.visible) {
        draw_numeric_axis(p, origin, plot, g.y_scale, ay, font, grid, axis);
    }
    // x 轴：把数值刻度标在绘图区下方（无类目标签，用数值刻度代替）
    if (ax.visible && plot.size.width > 0.0F) {
        const int digits = tick_digits(g.x_scale.step());
        for (const double t : g.x_scale.ticks()) {
            const float x = g.x_scale.to_px(t, plot.origin.x, plot.right());
            if (x < plot.origin.x - 0.5F || x > plot.right() + 0.5F) {
                continue;
            }
            if (ax.show_grid_lines) {
                p.draw_line(Point{.x = origin.x + x, .y = origin.y + plot.origin.y},
                            Point{.x = origin.x + x, .y = origin.y + plot.bottom()}, 1.0F, grid);
            }
            const std::string tick_text = format_number(t, Locale{}, digits);
            const float w = render::FontEngine::measure_width(tick_text, font);
            const Rect box{.origin = Point{.x = origin.x + x - (w * 0.5F), .y = origin.y + plot.bottom() + 2.0F},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, tick_text, font, axis);
        }
        if (!ax.label.empty()) {
            const float w = render::FontEngine::measure_width(ax.label, font);
            const Rect box{.origin = Point{.x = origin.x + plot.origin.x + (plot.size.width * 0.5F) - (w * 0.5F),
                                           .y = origin.y + plot.bottom() + line_h + 2.0F},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, ax.label, font, axis);
        }
    }

    std::optional<Point> hovered_px;
    const auto grow_t = static_cast<float>(grow_.progress());  // grow-in：点半径 0 → 1
    for (std::size_t k = 0; k < secs.size(); ++k) {
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
        Color c = resolve_series_color(k, secs[k].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        const float radius = std::max(0.5F, secs[k].dot_radius * grow_t);
        for (std::size_t i = 0; i < secs[k].points.size(); ++i) {
            const ChartPoint &pt = secs[k].points[i];
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

    if (leg.visible) {
        for (std::size_t k = 0; k < g.legend_rects.size() && k < secs.size(); ++k) {
            const Rect &r = g.legend_rects[k];
            const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
            Color c = resolve_series_color(k, secs[k].color, theme);
            if (dimmed) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = origin.x + r.origin.x, .y = origin.y + r.origin.y + 3.0F},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(swatch, 2.0F, c);
            const Rect box{.origin = Point{.x = origin.x + r.origin.x + 14.0F, .y = origin.y + r.origin.y},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(box, secs[k].name, font, axis);
        }
    }

    if (hovered_px.has_value()) {
        if (crosshair) {
            p.draw_line(Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.origin.y},
                        Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.bottom()}, 1.0F, grid);
            p.draw_line(Point{.x = origin.x + plot.origin.x, .y = origin.y + hovered_px->y},
                        Point{.x = origin.x + plot.right(), .y = origin.y + hovered_px->y}, 1.0F, grid);
        }
        constexpr float d = 12.0F;
        const Rect ring{
            .origin = Point{.x = origin.x + hovered_px->x - (d * 0.5F), .y = origin.y + hovered_px->y - (d * 0.5F)},
            .size = Size{.width = d, .height = d}};
        p.draw_rounded_border(ring, d * 0.5F, 2.0F, theme.roles.on_surface);

        if (hovered_point_.has_value()) {
            const auto [k, i] = *hovered_point_;
            if (k >= 0 && static_cast<std::size_t>(k) < secs.size() &&
                static_cast<std::size_t>(i) < secs[static_cast<std::size_t>(k)].points.size()) {
                const ChartPoint &pt = secs[static_cast<std::size_t>(k)].points[static_cast<std::size_t>(i)];
                const std::string text = secs[static_cast<std::size_t>(k)].name + ": (" + std::to_string(pt.x) + ", " +
                                         std::to_string(pt.y) + ")";
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                const float bx = std::clamp(hovered_px->x + 8.0F, 0.0F, std::max(0.0F, bounds.size.width - w));
                const float by = std::clamp(hovered_px->y - h - 4.0F, 0.0F, std::max(0.0F, bounds.size.height - h));
                const Rect box{.origin = Point{.x = origin.x + bx, .y = origin.y + by},
                               .size = Size{.width = w, .height = h}};
                p.fill_rounded_rect(box, 4.0F,
                                    Color{theme.roles.surface.r, theme.roles.surface.g, theme.roles.surface.b, 242});
                p.draw_rounded_border(box, 4.0F, 1.0F, grid);
                const Rect text_box{.origin = Point{.x = origin.x + bx + 8.0F, .y = origin.y + by + 4.0F},
                                    .size = Size{.width = w - 16.0F, .height = line_h}};
                p.draw_text(text_box, text, font, theme.roles.on_surface);
            }
        }
    }
}

}  // namespace aurora
