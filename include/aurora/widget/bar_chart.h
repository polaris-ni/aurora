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

/// @brief Aurora 命名空间：柱状图控件 BarChart 与其 header-only 实现。
namespace aurora {

/// @brief BarChart 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 十行皆为**可空值件 + 常量档回落点**：柱状图没有随作用域而变的属性（轴与网格的配色取自继承
/// 主题、不属属性面），故十枚读件一律无参；「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前十行都是带声明处默认值的裸字段；现在声明处默认值逐个落成各行回落基线（`series` /
/// `categories` 的基线就是空序列，`axis_x` / `axis_y` / `legend` 的基线是各自默认构造），生效值逐值不变。
///
/// 档位按「哪几趟消费本行」给出，与迁移前逐枚 setter 手写的标脏口径有六行不同，其中只有两行是行为修正：
/// `stacked` 与 `bar_width_ratio` 除绘制外还进 `compute_y_scale` / `compute_geometry`（布局期算定），故在绘制档
/// 之外再补布局档 —— 迁移前只标绘制会让几何缓存停在旧值，要等下一次无关重排才追上。另四行（`axis_x` /
/// `axis_y` / `legend` / `padding`）迁移前只标布局：前三枚在绘制趟被直接读，故补绘制档；`padding` 的值只进
/// 布局期的 `compute_geometry`、绘制趟读的是那趟缓存的几何，此处仍随 `LineChart` 的同键口径带上绘制档，
/// 因为「布局档是否必然带来重绘」取决于帧循环的脏区口径，本表不依赖那个推断。
///
/// 五枚结构化与数组行（`axis_x` / `axis_y` / `legend` 与 `series` / `categories`）走的是 `chart_common.h` 里
/// 那四枚编解码登记：类型名、JSON 类型与双向转换都由登记给出，本表只点名它们，
/// 而它们一律**未登记 `validate`** —— 迁移前那几条通路就是「字段缺失 / 类型不符逐项回退默认」的宽容
/// 口径，故这类行非法值不撤销表态、也不产生降级上报。两枚数值行（`bar_width_ratio` /
/// `bar_corner_radius`）则相反：`float` 登记自带按描述符约束的校验，故 JSON 里的越界值**不再夹取写入**，
/// 而是不写入并撤表态、上报降级；运行时写件仍按原样存入（约束由几何计算的夹取兜底），与迁移前一致。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class BarChartProps : public AbstractProps<BarChartProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：数据系列（多系列分组并排；单系列即普通柱状）。回落点是空序列。
    ///
    /// 档位取「布局 + 绘制」：域计算、带宽 / 柱宽几何与柱体三处都读它，不只是画。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto series_row() {
        return prop_row<&BarChartProps::series_>("series")
            .layout()
            .paint()
            .fallback(&BarChartProps::series_default)
            .note("Data series array: [{name, values:[double], color?}]");
    }

    /// @brief 可空值件：x 轴类目标签；缺省以序号 "1","2",… 补齐。回落点是空序列。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto categories_row() {
        return prop_row<&BarChartProps::categories_>("categories")
            .layout()
            .paint()
            .fallback(&BarChartProps::categories_default)
            .note("Category labels on the x axis; defaults to the index");
    }

    /// @brief 可空值件：堆叠模式开关（同 x 的多系列沿 y 累加）；回落基线为关。
    ///
    /// 本行同时改数值轴域（按类目求和）与单柱宽（堆叠时等于柱组宽），两者都在布局期算定。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto stacked_row() {
        return prop_row<&BarChartProps::stacked_>("stacked")
            .layout()
            .paint()
            .fallback(&BarChartProps::stacked_default)
            .note("Stacked mode (series at the same x accumulate)");
    }

    /// @brief 可空值件：柱宽占带比例；回落基线 0.7，约束区间 [0.05,1]（带宽非负退化时仍夹到下限）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto bar_width_ratio_row() {
        return prop_row<&BarChartProps::bar_width_ratio_>("bar_width_ratio")
            .layout()
            .paint()
            .bounds("0.05", "1")
            .fallback(&BarChartProps::bar_width_ratio_default)
            .note("Bar width as a share of the band, clamped to (0,1]");
    }

    /// @brief 可空值件：柱圆角（dp）；回落基线 2.0，下界 0（绘制时再自动不超过柱宽 / 柱高的一半）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto bar_corner_radius_row() {
        return prop_row<&BarChartProps::bar_corner_radius_>("bar_corner_radius")
            .paint()
            .bounds("0")
            .fallback(&BarChartProps::bar_corner_radius_default)
            .note("Bar corner radius (dp), capped at half the bar width/height");
    }

    /// @brief 可空值件：悬停十字准线开关（吸附最近类目）；回落基线为开。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_crosshair_row() {
        return prop_row<&BarChartProps::show_crosshair_>("show_crosshair")
            .paint()
            .fallback(&BarChartProps::show_crosshair_default)
            .note("Draw a crosshair snapped to the nearest category on hover");
    }

    /// @brief 可空值件：类目轴规格。回落点是默认构造的 `ChartAxisSpec`（可见、五刻度、画网格）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_x_row() {
        return prop_row<&BarChartProps::axis_x_>("axis_x")
            .layout()
            .paint()
            .fallback(&BarChartProps::axis_x_default)
            .note("Category axis spec: {visible,label,tick_count,min,max,show_grid_lines,include_zero}");
    }

    /// @brief 可空值件：数值轴（Linear）规格。回落点是默认构造的 `ChartAxisSpec`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_y_row() {
        return prop_row<&BarChartProps::axis_y_>("axis_y")
            .layout()
            .paint()
            .fallback(&BarChartProps::axis_y_default)
            .note("Value axis spec: {visible,label,tick_count,min,max,show_grid_lines,include_zero}");
    }

    /// @brief 可空值件：图例规格。回落点是默认构造的 `ChartLegendSpec`（可见、顶部）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto legend_row() {
        return prop_row<&BarChartProps::legend_>("legend")
            .layout()
            .paint()
            .fallback(&BarChartProps::legend_default)
            .note("Legend spec: {visible,position:Top|Bottom|Right}");
    }

    /// @brief 可空值件：图内留白（dp），轴标签 / 值框的避让区。回落基线四边各 8.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&BarChartProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&BarChartProps::padding_default)
            .note("In-chart padding (dp), reserved for axis labels and value boxes");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(series_row(), categories_row(), stacked_row(), bar_width_ratio_row(),
                              bar_corner_radius_row(), show_crosshair_row(), axis_x_row(), axis_y_row(), legend_row(),
                              padding_row());
    }

    // ---- 读面：每行恰一件，十行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的数据系列。
    /// @return 生效序列（数组行按值给出：未表态时为空序列）。
    [[nodiscard]] auto series() const -> std::vector<ChartSeries> { return stated(series_row()); }

    /// @brief 当前生效的 x 轴类目标签。
    /// @return 生效标签（未表态时为空序列；补齐到类目数是控件侧 `resolved_categories` 的事）。
    [[nodiscard]] auto categories() const -> std::vector<std::string> { return stated(categories_row()); }

    /// @brief 当前生效的堆叠模式开关。
    /// @return 生效开关。
    [[nodiscard]] auto stacked() const -> bool { return stated(stacked_row()); }

    /// @brief 当前生效的柱宽占带比例。
    /// @return 生效占比（几何计算时夹取 [0.05,1]）。
    [[nodiscard]] auto bar_width_ratio() const -> float { return stated(bar_width_ratio_row()); }

    /// @brief 当前生效的柱圆角（dp）。
    /// @return 生效半径。
    [[nodiscard]] auto bar_corner_radius() const -> float { return stated(bar_corner_radius_row()); }

    /// @brief 当前生效的十字准线开关。
    /// @return 生效开关。
    [[nodiscard]] auto show_crosshair() const -> bool { return stated(show_crosshair_row()); }

    /// @brief 当前生效的类目轴规格。
    /// @return 生效规格（未表态时为默认构造）。
    [[nodiscard]] auto axis_x() const -> ChartAxisSpec { return stated(axis_x_row()); }

    /// @brief 当前生效的数值轴规格。
    /// @return 生效规格（未表态时为默认构造）。
    [[nodiscard]] auto axis_y() const -> ChartAxisSpec { return stated(axis_y_row()); }

    /// @brief 当前生效的图例规格。
    /// @return 生效规格（未表态时为可见、顶部）。
    [[nodiscard]] auto legend() const -> ChartLegendSpec { return stated(legend_row()); }

    /// @brief 当前生效的图内留白（dp）。
    /// @return 生效留白。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入数据系列；表态即脱离空序列基线。
    /// @param value 新系列（多系列分组并排；单系列即普通柱状）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_series(const std::vector<ChartSeries> &value) -> BarChartProps & { return assign(series_row(), value); }

    /// @brief 装入 x 轴类目标签。
    /// @param value 类目标签列表；不足类目数处以序号补齐。
    /// @return 引用本属性对象，便于链式续写。
    auto set_categories(const std::vector<std::string> &value) -> BarChartProps & {
        return assign(categories_row(), value);
    }

    /// @brief 装入堆叠模式开关。
    /// @param value true 时同 x 位各系列沿 y 累加；false 时分组并排。
    /// @return 引用本属性对象，便于链式续写。
    auto set_stacked(bool value) -> BarChartProps & { return assign(stacked_row(), value); }

    /// @brief 装入柱宽占带比例。
    /// @param value 新占比；越界值运行时照存，由几何计算夹取兜底。
    /// @return 引用本属性对象，便于链式续写。
    auto set_bar_width_ratio(float value) -> BarChartProps & { return assign(bar_width_ratio_row(), value); }

    /// @brief 装入柱圆角（dp）；绘制时自动不超过柱宽 / 柱高的一半。
    /// @param value 新半径。
    /// @return 引用本属性对象，便于链式续写。
    auto set_bar_corner_radius(float value) -> BarChartProps & { return assign(bar_corner_radius_row(), value); }

    /// @brief 装入十字准线开关。
    /// @param value true 时悬停吸附最近类目绘制垂直准线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_crosshair(bool value) -> BarChartProps & { return assign(show_crosshair_row(), value); }

    /// @brief 装入类目轴（Band）规格。
    /// @param value 轴规格（可见性 / 标签 / 刻度数 / 显式域等）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_x(const ChartAxisSpec &value) -> BarChartProps & { return assign(axis_x_row(), value); }

    /// @brief 装入数值轴（Linear）规格。
    /// @param value 轴规格（可见性 / 标签 / 刻度数 / 显式域 / include_zero 等）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_y(const ChartAxisSpec &value) -> BarChartProps & { return assign(axis_y_row(), value); }

    /// @brief 装入图例规格。
    /// @param value 图例规格（可见性 / 位置）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_legend(const ChartLegendSpec &value) -> BarChartProps & { return assign(legend_row(), value); }

    /// @brief 装入图内留白（dp）。
    /// @param value 四边留白。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> BarChartProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 数据系列回落基线。
    /// @return 空序列（无数据 ⇒ 只画轴不画柱）。
    [[nodiscard]] static auto series_default() -> std::vector<ChartSeries> { return {}; }

    /// @brief 类目标签回落基线。
    /// @return 空序列（⇒ 全部类目以序号补齐）。
    [[nodiscard]] static auto categories_default() -> std::vector<std::string> { return {}; }

    /// @brief 堆叠模式回落基线。
    /// @return false（默认分组并排）。
    [[nodiscard]] static auto stacked_default() -> bool { return false; }

    /// @brief 柱宽占带比例回落基线。
    /// @return 0.7F（带宽的七成）。
    [[nodiscard]] static auto bar_width_ratio_default() -> float { return 0.7F; }

    /// @brief 柱圆角回落基线。
    /// @return 2.0F（dp）。
    [[nodiscard]] static auto bar_corner_radius_default() -> float { return 2.0F; }

    /// @brief 十字准线开关回落基线。
    /// @return true（默认画准线）。
    [[nodiscard]] static auto show_crosshair_default() -> bool { return true; }

    /// @brief 类目轴回落基线。
    /// @return 默认构造的轴规格（可见、五刻度、画网格、域含 0）。
    [[nodiscard]] static auto axis_x_default() -> ChartAxisSpec { return {}; }

    /// @brief 数值轴回落基线。
    /// @return 默认构造的轴规格（无显式域 ⇒ 域按数据 nice 化；柱的零基线靠 `include_zero`）。
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
    std::optional<std::vector<ChartSeries>> series_;  ///< 数据系列表态位
    std::optional<std::vector<std::string>> categories_;  ///< 类目标签表态位
    std::optional<bool> stacked_;  ///< 堆叠模式表态位
    std::optional<float> bar_width_ratio_;  ///< 柱宽占带比例表态位
    std::optional<float> bar_corner_radius_;  ///< 柱圆角表态位（dp）
    std::optional<bool> show_crosshair_;  ///< 十字准线开关表态位
    std::optional<ChartAxisSpec> axis_x_;  ///< 类目轴规格表态位
    std::optional<ChartAxisSpec> axis_y_;  ///< 数值轴规格表态位
    std::optional<ChartLegendSpec> legend_;  ///< 图例规格表态位
    std::optional<EdgeInsets> padding_;  ///< 图内留白表态位（dp）

    friend class AbstractProps<BarChartProps>;
};

/// @brief 柱状图控件（叶控件；契约见 specification/04-widget.md §3.8）。
///
/// 纯值属性驱动（`BarChartProps`），数据进序列化面，绘制全部经软件 `Painter`：
/// 柱体 = `fill_rounded_rect`、网格 / 轴 = `draw_line` + `draw_text`、悬停值框自绘。
/// 轴域与命中反查同源于 `LinearScale` / `BandScale`，故悬停命中的类目与渲染一致。
///
/// 属性一律经 `BarChartProps` 的读写件访问（本类公有继承它，故 `chart.set_stacked(true)` 与
/// `chart.bar_width_ratio()` 都直接可用）；十行皆常量档，读件一律无参。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class BarChart : public LeafWidget, public BarChartProps {
  public:
    /// @brief 默认构造：十行皆未表态（生效值即各行回落基线）；并接线宿主。
    BarChart() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 柱状图属性对象，移动进本实例的 `BarChartProps` 基类子对象。
    explicit BarChart(BarChartProps props) : BarChartProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `BarChartProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> BarChartProps { return {}; }

    /// @brief 点击（抬起）命中数据柱时触发：`(系列索引, 类目索引)`。旁挂，不进序列化面。
    /// @param series_idx 回调形参：命中的数据系列索引。
    /// @param point_idx 回调形参：命中的类目索引。
    /// @return 回调对象本身；未挂载回调时为空 `std::function`。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    /// @brief 装入数据系列：转调属性面写件，再重播 grow-in 入场动画。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的写件：动画重播是控件自身的事实，不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值仍由同一行存储与回落给出）。
    /// @param value 新系列（多系列分组并排；单系列即普通柱状）。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_series(const std::vector<ChartSeries> &value) -> BarChart & {
        BarChartProps::set_series(value);
        grow_.replay();  // 重播 grow-in 入场动画（未接帧循环时为 no-op）
        return *this;
    }

    /// @brief 类型名。
    /// @return 固定为 "BarChart"。
    [[nodiscard]] auto type_name() const -> const char * override { return "BarChart"; }

    /// @brief 静态描述符入口：属性矩阵由权威表派生，事件 / 不变式 / 样例留在本处。
    /// @return WidgetDescriptor（含 bar_width_ratio ∈ [0.05,1]、padding ≥ 0 等不变式）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("BarChart");
        descriptor.events = {"on_point_tapped"};
        descriptor.children_policy = "none";
        descriptor.invariants = {"0 < bar_width_ratio <= 1", "padding >= 0"};
        descriptor.examples = {
            "au::BarChart(au::BarChartProps().set_series({{ .name = \"A\", .values = {1, 2, 3} }}))"};
        return descriptor;
    }
    /// @brief 返回控件描述符，与 describe_static 同源。
    /// @return 同 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集可观察信号：登记各行已绑定的外部来源（本控件十行皆常量档，现状为空）。
    /// @param out 收集输出容器，追加来源视图。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 序列化自有属性到 `props`：先由基类写通用属性，再写各行；未表态的行不写键。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 已登记校验的行（`bar_width_ratio` 越出 [0.05,1] / `bar_corner_radius` 为负）非法值一律
    ///       不写入并撤表态；系列 / 类目 / 轴 / 图例 / 留白按既有宽容口径逐项回退默认（不登记校验，
    ///       故照常表态且不上报）。
    /// @param props 源 JSON 对象，按权威表逐行取键。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 图表自带点击目标语义：始终参与点击（数据柱命中即触发回调）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停离开时清除高亮（基类默认只置 `hover_` 不标脏，必须覆写）。
    /// @param entered true 进入悬停；false 离开（清数据点 / 图例高亮并标脏绘制）。
    auto on_hover_change(bool entered) -> void override {
        hover_ = entered;
        if (!entered && (hovered_point_.has_value() || legend_hover_.has_value())) {
            hovered_point_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    /// @brief 消费指针事件：Move 先查图例命中（优先于数据区）并更新图例 / 数据点悬停高亮，
    ///        Release 命中柱体触发 on_point_tapped。
    /// @param e 鼠标事件；Move / Release 被消费（置 `is_handled`），其余回落基类。
    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`—— 推断表不识 `BarChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @return 统一为 `AccessibilityRole::Image`。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    /// @brief 无障碍标签：图类型与规模（数据本身不进语义树，避免读屏念出一长串数字）。
    /// @return 形如 "BarChart, N series, M categories"。
    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值：当前悬停 / 选中的数据点（未悬停时为空）。
    /// @return 悬停点的 "系列名: 数值" 文本；未悬停或索引越界为空串。
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 类目索引）；无悬停为空。测试与调试用。
    /// @return 悬停点 (系列索引, 类目索引)；未命中为 `std::nullopt`。
    [[nodiscard]] auto hovered_point() const -> std::optional<std::pair<int, int>> { return hovered_point_; }
    /// @brief 当前悬停的图例项索引；无悬停为空。
    /// @return 图例项索引；未命中为 `std::nullopt`。
    [[nodiscard]] auto hovered_legend() const -> std::optional<std::size_t> { return legend_hover_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态）。
    /// @param ctx 构建上下文（当前实现未使用）。
    auto on_mount([[maybe_unused]] const BuildContext &ctx) -> void override { grow_.mount(); }

  private:
    /// @brief 布局期算定的绘图几何（局部坐标，原点 0）：渲染与命中反查共用同一份。
    struct Geometry {
        Rect plot{};  ///< 柱体绘制区（不含轴留白）
        LinearScale y_scale;  ///< 数值轴
        BandScale x_band{0};  ///< 类目轴
        std::vector<std::string> cats;  ///< 解析后的类目标签（缺省以序号补齐）
        float band_w = 0.0F;  ///< 单类目带宽
        float group_w = 0.0F;  ///< 带内柱组总宽
        float bar_w = 0.0F;  ///< 单柱宽（堆叠时 = group_w）
        std::vector<Rect> legend_rects;  ///< 图例项命中区（局部坐标）
    };

    /// @brief 解析生效类目标签：显式标签不足处以序号字符串 "1","2",… 补齐到类目数。
    /// @param data 本趟取出的数据系列（数组行按值读出，故由调用方取一次再传下来）。
    /// @param labels 本趟取出的类目标签。
    /// @return 与类目数等长的标签列表。
    [[nodiscard]] static auto resolved_categories(const std::vector<ChartSeries> &data,
                                                  const std::vector<std::string> &labels) -> std::vector<std::string>;
    /// @brief 类目数 = max(显式类目标签数, 各系列 values 的最大长度)。
    /// @param data 本趟取出的数据系列。
    /// @param labels 本趟取出的类目标签。
    /// @return 类目总数。
    [[nodiscard]] static auto category_count(const std::vector<ChartSeries> &data,
                                             const std::vector<std::string> &labels) -> std::size_t;
    /// @brief 计算数值轴域：堆叠模式按类目求和取域，空数据退化 [0,1]，axis_y.min/max 显式覆盖，include_zero 并入 0。
    /// @param data 本趟取出的数据系列。
    /// @param cats 本趟解析出的类目标签（长度即类目数，堆叠求和按它逐项展开）。
    /// @param stack 本趟取出的堆叠模式开关（决定按类目求和还是逐值取域）。
    /// @return 带刻度文本的 LinearScale。
    [[nodiscard]] auto compute_y_scale(const std::vector<ChartSeries> &data, const std::vector<std::string> &cats,
                                       bool stack) const -> LinearScale;
    /// @brief 由尺寸与字体算定绘图几何：扣除图例带与轴标签留白得绘图区，再算带宽 / 柱组宽
    ///        （bar_width_ratio 夹取 [0.05,1]）/ 单柱宽与图例命中区。
    /// @param size 控件尺寸。
    /// @param font 继承主题的字体。
    /// @return 渲染与命中反查共用的 Geometry（同源）。
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    /// @brief 反查局部坐标命中的数据点：先限绘图区内，堆叠按 y 分段、分组按柱宽切片定位系列。
    /// @param local 控件局部坐标。
    /// @return (系列索引, 类目索引)；未命中为 `std::nullopt`。
    [[nodiscard]] auto hit_test_point(const Point &local) const -> std::optional<std::pair<int, int>>;
    /// @brief 反查命中的图例项（命中区与绘制同源于 geom_.legend_rects）。
    /// @param g 布局期几何。
    /// @param local 控件局部坐标。
    /// @return 图例项索引；未命中为 `std::nullopt`。
    [[nodiscard]] static auto legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t>;
    /// @brief 安全取数据点值：系列 / 类目越界或非有限值（NaN / Inf）回退 0.0。
    /// @param data 本趟取出的数据系列（数组行按值读出，热点循环里绝不逐点重取）。
    /// @param series_idx 系列索引。
    /// @param cat_idx 类目索引。
    /// @return 数据点值。
    [[nodiscard]] static auto series_value(const std::vector<ChartSeries> &data, std::size_t series_idx,
                                           std::size_t cat_idx) -> double;
    /// @brief 类目带的中心 x 坐标（局部坐标）。
    /// @param g 布局期几何。
    /// @param cat_idx 类目索引。
    /// @return 带中心像素位置。
    [[nodiscard]] static auto category_center(const Geometry &g, std::size_t cat_idx) -> float;

    Geometry geom_;  ///< 布局期算定的绘图几何缓存（on_layout 写入，绘制 / 命中反查复用）
    ChartGrowIn grow_;  ///< grow-in 入场动画状态（on_mount 接入帧循环；数据变更时重放）
    std::optional<std::pair<int, int>> hovered_point_;  ///< 当前悬停的数据点（系列, 类目）；无悬停为空
    std::optional<std::size_t> legend_hover_;  ///< 当前悬停的图例项索引；无悬停为空
};

// ---------------------------------------------------------------------------
// 实现（header-only：与既有 ProgressIndicator 同形态，逻辑全部为纯几何 / 纯值计算）
// ---------------------------------------------------------------------------

inline auto BarChart::category_count(const std::vector<ChartSeries> &data, const std::vector<std::string> &labels)
    -> std::size_t {
    std::size_t n = labels.size();
    for (const ChartSeries &s : data) {
        n = std::max(n, s.values.size());
    }
    return n;
}

inline auto BarChart::resolved_categories(const std::vector<ChartSeries> &data, const std::vector<std::string> &labels)
    -> std::vector<std::string> {
    const std::size_t n = category_count(data, labels);
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(i < labels.size() ? labels[i] : std::to_string(i + 1));
    }
    return out;
}

inline auto BarChart::series_value(const std::vector<ChartSeries> &data, std::size_t series_idx, std::size_t cat_idx)
    -> double {
    if (series_idx >= data.size()) {
        return 0.0;
    }
    const std::vector<double> &vals = data[series_idx].values;
    if (cat_idx >= vals.size()) {
        return 0.0;
    }
    return std::isfinite(vals[cat_idx]) ? vals[cat_idx] : 0.0;
}

// min 与 max 同时显式时走 from_explicit，否则 from_domain 取整刻度；空数据退化 [0,1]。
inline auto BarChart::compute_y_scale(const std::vector<ChartSeries> &data, const std::vector<std::string> &cats,
                                      bool stack) const -> LinearScale {
    const std::size_t n_cat = cats.size();
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    if (stack) {
        // 堆叠：每个类目的有效上界 = 该类目各系列之和。
        for (std::size_t j = 0; j < n_cat; ++j) {
            double sum = 0.0;
            for (std::size_t i = 0; i < data.size(); ++i) {
                sum += series_value(data, i, j);
            }
            lo = have ? std::min(lo, sum) : sum;
            hi = have ? std::max(hi, sum) : sum;
            have = true;
        }
    } else {
        for (const ChartSeries &s : data) {
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
        hi = 1.0;  // 空数据：域退化为 [0,1]，只画轴不画柱
    }
    const ChartAxisSpec y_axis = axis_y();  // 结构化行按值读出，本趟只取一次
    if (y_axis.min.has_value()) {
        lo = *y_axis.min;
    }
    if (y_axis.max.has_value()) {
        hi = *y_axis.max;
    }
    if (y_axis.include_zero) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    if (y_axis.min.has_value() && y_axis.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, y_axis.tick_count);
    }
    return LinearScale::from_domain(lo, hi, y_axis.tick_count);
}

// 轴可见性与刻度文本宽度决定左右 / 底部预留；图例命中区按与绘制同一套游标规则生成。
inline auto BarChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    // 八行在本函数里逐档多次消费，数组行与结构化行皆按值读出 ⇒ 本趟各取一次再复用。
    const std::vector<ChartSeries> data = series();
    const std::vector<std::string> labels = categories();
    const EdgeInsets pad = padding();
    const ChartLegendSpec leg = legend();
    const ChartAxisSpec x_axis = axis_x();
    const ChartAxisSpec y_axis = axis_y();
    const bool stack = stacked();
    const float width_ratio = bar_width_ratio();

    Geometry g;
    g.cats = resolved_categories(data, labels);
    g.y_scale = compute_y_scale(data, g.cats, stack);
    g.x_band = BandScale{g.cats.size()};

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = pad.left;
    float top = pad.top;
    float right = std::max(pad.left, size.width - pad.right);
    float bottom = std::max(pad.top, size.height - pad.bottom);

    // 图例带（先扣，再算轴留白）
    if (leg.visible && !data.empty()) {
        if (leg.position == LegendPosition::Top) {
            top += line_h;
        } else if (leg.position == LegendPosition::Bottom) {
            bottom -= line_h;
        } else {
            float widest = 0.0F;
            for (const ChartSeries &s : data) {
                widest = std::max(widest, render::FontEngine::measure_width(s.name, font));
            }
            right = std::max(left, right - (widest + 22.0F));
        }
    }

    // x 轴（类目标签 + 轴标题）
    if (x_axis.visible) {
        bottom -= line_h;
        if (!x_axis.label.empty()) {
            bottom -= line_h;
        }
    }
    // y 轴（刻度标签 + 轴标题）：标签宽度取决于格式化后的刻度文本
    if (y_axis.visible) {
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
        if (!y_axis.label.empty()) {
            left += line_h;
        }
    }

    g.plot = Rect{.origin = Point{.x = left, .y = top},
                  .size = Size{.width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)}};
    const std::size_t n_cat = g.cats.size();
    g.band_w = n_cat == 0U ? 0.0F : (g.plot.size.width / static_cast<float>(n_cat));
    const float ratio = std::clamp(width_ratio, 0.05F, 1.0F);
    g.group_w = g.band_w * ratio;
    const std::size_t n_series = data.size();
    g.bar_w = (stack || n_series == 0U) ? g.group_w : (g.group_w / static_cast<float>(n_series));

    // 图例项命中区（与绘制同一套游标推进规则；命中优先于数据区）
    if (leg.visible && !data.empty()) {
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
        for (const ChartSeries &s : data) {
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

// 供十字准线吸附与类目标签居中使用。
inline auto BarChart::category_center(const Geometry &g, std::size_t cat_idx) -> float {
    return g.x_band.band_center_px(cat_idx, g.plot.origin.x, g.plot.right());
}

// 对 legend_rects 逐项做矩形包含判断。
inline auto BarChart::legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

/// @brief 布局：填充最大约束（无界时退化 300×200 缺省画布），非负化后按主题字体算定并缓存 geom_。
/// @param c 来自父级的约束。
/// @param ctx 构建上下文（继承字体参与几何预留）。
/// @return 采用的控件尺寸。
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
    geom_ = compute_geometry(s, inherit_theme(ctx).type_scale.body_medium);
    return s;
}

// 堆叠模式自上而下取第一个包含该 y 的累加分段定位系列。
inline auto BarChart::hit_test_point(const Point &local) const -> std::optional<std::pair<int, int>> {
    // 命中反查逐段遍历全部系列：数组行按值读出，故本趟只取一次。
    const std::vector<ChartSeries> data = series();
    const bool stack = stacked();
    const Rect &plot = geom_.plot;
    if (plot.size.width <= 0.0F || plot.size.height <= 0.0F || geom_.cats.empty() || data.empty()) {
        return std::nullopt;
    }
    if (local.x < plot.origin.x || local.x > plot.right() || local.y < plot.origin.y || local.y > plot.bottom()) {
        return std::nullopt;
    }
    const std::size_t j = geom_.x_band.index_at(local.x, plot.origin.x, plot.right());
    std::size_t i = 0;
    if (!stack && data.size() > 1U) {
        const float center = geom_.x_band.band_center_px(j, plot.origin.x, plot.right());
        const float group_left = center - (geom_.group_w * 0.5F);
        const float t = geom_.bar_w > 0.0F ? (local.x - group_left) / geom_.bar_w : 0.0F;
        const auto raw = static_cast<long long>(std::floor(t));
        i = (raw < 0) ? 0U : std::min(static_cast<std::size_t>(raw), data.size() - 1U);
    } else if (stack) {
        // 堆叠：按 y 落在哪一段决定系列（自上而下取第一个包含该 y 的段）
        const float y0 = geom_.y_scale.to_px(0.0, plot.bottom(), plot.origin.y);
        for (std::size_t k = 0; k < data.size(); ++k) {
            double acc = 0.0;
            for (std::size_t m = 0; m <= k; ++m) {
                acc += series_value(data, m, j);
            }
            const float y_top = geom_.y_scale.to_px(acc, plot.bottom(), plot.origin.y);
            const float y_prev = geom_.y_scale.to_px(acc - series_value(data, k, j), plot.bottom(), plot.origin.y);
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
    const std::vector<ChartSeries> data = series();
    const std::vector<std::string> labels = categories();
    return "BarChart, " + std::to_string(data.size()) + " series, " + std::to_string(category_count(data, labels)) +
           " categories";
}

inline auto BarChart::accessibility_value() const -> std::string {
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const std::vector<ChartSeries> data = series();
    const auto [i, j] = *hovered_point_;
    if (i < 0 || j < 0 || static_cast<std::size_t>(i) >= data.size()) {
        return std::string{};
    }
    return data[static_cast<std::size_t>(i)].name + ": " +
           std::to_string(series_value(data, static_cast<std::size_t>(i), static_cast<std::size_t>(j)));
}

/// @brief 绘制：y 轴网格、柱体（grow-in 进度缩放、图例联动降透明、越界裁剪）、轴刻度与标签、
///        图例（与 geom_.legend_rects 同源）、悬停高亮边框 / 十字准线 / 自绘值框（一律夹进 bounds）。
/// @param p 软件绘制器。
/// @param bounds 控件全局矩形（局部几何经偏移换算为全局坐标）。
/// @param ctx 构建上下文（继承主题字体 / 配色）。
inline auto BarChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme &theme = inherit_theme(ctx);
    const Font &font = theme.type_scale.body_medium;
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
    const Color grid =
        Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 31};  // ≈ text × 0.12
    const Color axis =
        Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 160};  // ≈ text × 0.63

    // 绘制档各行本帧只读一次：数组行与结构化行皆按值给出，逐柱循环里绝不重取。
    const std::vector<ChartSeries> data = series();
    const ChartAxisSpec x_axis = axis_x();
    const ChartAxisSpec y_axis = axis_y();
    const ChartLegendSpec leg = legend();
    const bool stack = stacked();
    const float corner = bar_corner_radius();
    const bool crosshair = show_crosshair();

    // ---- 网格线（y 刻度）----
    if (y_axis.visible && y_axis.show_grid_lines && plot.size.height > 0.0F) {
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
    const std::size_t n_series = data.size();
    std::vector<double> stack_acc(n_cat, 0.0);
    std::optional<Rect> hovered_bar;
    const double grow_t = grow_.progress();  // grow-in：柱高 0 → 1（无 Animator 时恒 1）
    for (std::size_t i = 0; i < n_series; ++i) {
        Color c = resolve_series_color(i, data[i].color, theme);
        // 图例联动：命中某图例项时，其余系列降透明（0.35）
        if (legend_hover_.has_value() && (*legend_hover_ != i)) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        for (std::size_t j = 0; j < n_cat; ++j) {
            const double v = series_value(data, i, j) * grow_t;
            const double lo = stack ? stack_acc[j] : 0.0;
            const double hi = stack ? stack_acc[j] + v : v;
            if (stack) {
                stack_acc[j] = hi;
            }
            const float y_hi = g.y_scale.to_px(hi, plot.bottom(), plot.origin.y);
            const float y_lo = g.y_scale.to_px(lo, plot.bottom(), plot.origin.y);
            const float top = std::min(y_lo, y_hi);
            const float bottom = std::max(y_lo, y_hi);
            // 夹进绘图区：域外的值不得画到轴外
            const float clipped_top = std::max(top, plot.origin.y);
            const float clipped_bottom = std::min(bottom, plot.bottom());
            if (clipped_bottom - clipped_top <= 0.0F) {
                continue;
            }
            const float center = g.x_band.band_center_px(j, plot.origin.x, plot.right());
            const float group_left = center - (g.group_w * 0.5F);
            const float x = stack ? group_left : (group_left + (static_cast<float>(i) * g.bar_w));
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
            const float radius = std::min(corner, std::min(bar.size.width, bar.size.height) * 0.5F);
            if (radius > 0.0F) {
                p.fill_rounded_rect(grect(bar), radius, c);
            } else {
                p.fill_rect(grect(bar), c);
            }
            if (hovered_point_.has_value() && std::cmp_equal(hovered_point_->first, i) &&
                std::cmp_equal(hovered_point_->second, j)) {
                hovered_bar = bar;
            }
        }
    }

    // ---- y 轴刻度标签 ----
    if (y_axis.visible && plot.size.height > 0.0F) {
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
        if (!y_axis.label.empty()) {
            const float w = render::FontEngine::measure_width(y_axis.label, font);
            const Rect box{.origin = Point{.x = gx(plot.origin.x - w - 6.0F - line_h), .y = gy(plot.origin.y)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, y_axis.label, font, axis);
        }
    }

    // ---- x 轴类目标签 ----
    if (x_axis.visible && n_cat > 0U && plot.size.width > 0.0F) {
        for (std::size_t j = 0; j < n_cat; ++j) {
            const std::string &s = g.cats[j];
            const float w = render::FontEngine::measure_width(s, font);
            const float center = g.x_band.band_center_px(j, plot.origin.x, plot.right());
            const Rect box{.origin = Point{.x = gx(center - (w * 0.5F)), .y = gy(plot.bottom() + 2.0F)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, s, font, axis);
        }
        if (!x_axis.label.empty()) {
            const float w = render::FontEngine::measure_width(x_axis.label, font);
            const Rect box{.origin = Point{.x = gx(plot.origin.x + (plot.size.width * 0.5F) - (w * 0.5F)),
                                           .y = gy(plot.bottom() + line_h + 2.0F)},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, x_axis.label, font, axis);
        }
    }

    // ---- 图例（命中区与绘制同源于 geom_.legend_rects）----
    if (leg.visible) {
        for (std::size_t i = 0; i < g.legend_rects.size() && i < n_series; ++i) {
            const Rect &r = g.legend_rects[i];
            Color c = resolve_series_color(i, data[i].color, theme);
            if (legend_hover_.has_value() && (*legend_hover_ != i)) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = gx(r.origin.x), .y = gy(r.origin.y + 3.0F)},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(grect(swatch), 2.0F, c);
            const Rect box{.origin = Point{.x = gx(r.origin.x + 14.0F), .y = gy(r.origin.y)},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(grect(box), data[i].name, font, axis);
        }
    }

    // ---- 悬停高亮 + 十字准线 + 自绘值框 ----
    if (hovered_bar.has_value()) {
        // 十字准线：吸附最近类目的垂直参考线
        if (crosshair && hovered_point_.has_value() && hovered_point_->second >= 0) {
            const float cx = category_center(g, static_cast<std::size_t>(hovered_point_->second));
            p.draw_line(Point{.x = gx(cx), .y = gy(plot.origin.y)}, Point{.x = gx(cx), .y = gy(plot.bottom())}, 1.0F,
                        grid);
        }
        p.draw_rounded_border(grect(*hovered_bar), 2.0F, 2.0F, theme.roles.on_surface);
        if (hovered_point_.has_value()) {
            const auto [si, ci] = *hovered_point_;
            if (si >= 0 && std::cmp_less(si, n_series)) {
                const std::string text =
                    data[static_cast<std::size_t>(si)].name + ": " +
                    std::to_string(series_value(data, static_cast<std::size_t>(si), static_cast<std::size_t>(ci)));
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                // 值框一律夹在控件 bounds 内（越界像素不会被脏区擦除 → 残影）
                float bx = hovered_bar->origin.x + (hovered_bar->size.width * 0.5F) - (w * 0.5F);
                float by = hovered_bar->origin.y - h - 4.0F;
                bx = std::clamp(bx, 0.0F, std::max(0.0F, bounds.size.width - w));
                by = std::clamp(by, 0.0F, std::max(0.0F, bounds.size.height - h));
                const Rect box{.origin = Point{.x = bx, .y = by}, .size = Size{.width = w, .height = h}};
                p.fill_rounded_rect(grect(box), 4.0F,
                                    Color{theme.roles.surface.r, theme.roles.surface.g, theme.roles.surface.b, 242});
                p.draw_rounded_border(grect(box), 4.0F, 1.0F, grid);
                const Rect text_box{.origin = Point{.x = bx + 8.0F, .y = by + 4.0F},
                                    .size = Size{.width = w - 16.0F, .height = line_h}};
                p.draw_text(grect(text_box), text, font, theme.roles.on_surface);
            }
        }
    }
}

}  // namespace aurora
