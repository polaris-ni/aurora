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

/// @brief Aurora 命名空间：折线图控件 LineChart 与其 header-only 实现。
namespace aurora {

/// @brief LineChart 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 十行皆为**可空值件 + 常量档回落点**：折线图没有随作用域而变的属性（轴与网格的配色取自继承
/// 主题、不属属性面），故十枚读件一律无参；「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前十行都是带声明处默认值的裸字段；现在声明处默认值逐个落成各行回落基线（`series` /
/// `categories` 的基线就是空序列，`axis_x` / `axis_y` / `legend` 的基线是各自默认构造），生效值逐值不变。
///
/// 四枚结构化行（`axis_x` / `axis_y` / `legend` 与数组行 `series` / `categories`）的编解码登记见
/// `chart_common.h`：类型名、JSON 类型与双向转换都由那几处登记给出，本表只点名它们，
/// 而它们一律**未登记 `validate`** —— 迁移前那几条通路就是「字段缺失 / 类型不符逐项回退默认」的宽容
/// 口径，故这类行非法值不撤销表态、也不产生降级上报。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class LineChartProps : public AbstractProps<LineChartProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：数据系列（`values` 等距，x = 索引）。回落点是空序列。
    ///
    /// 档位取「布局 + 绘制」：域计算、几何预留与描线三处都读它，不只是画。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto series_row() {
        return prop_row<&LineChartProps::series_>("series")
            .layout()
            .paint()
            .fallback(&LineChartProps::series_default)
            .note("Data series array (values equally spaced, x = index)");
    }

    /// @brief 可空值件：x 轴类目标签；缺省以序号 "1","2",… 补齐。回落点是空序列。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto categories_row() {
        return prop_row<&LineChartProps::categories_>("categories")
            .layout()
            .paint()
            .fallback(&LineChartProps::categories_default)
            .note("x axis category labels; defaults to the index");
    }

    /// @brief 可空值件：数据点圆点开关；回落基线为开。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_dots_row() {
        return prop_row<&LineChartProps::show_dots_>("show_dots")
            .paint()
            .fallback(&LineChartProps::show_dots_default)
            .note("Draw dots at the data points");
    }

    /// @brief 可空值件：折线宽度（dp）；回落基线 2.0，下界 0（0 是「显式要一条不画出来的线」）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto line_width_row() {
        return prop_row<&LineChartProps::line_width_>("line_width")
            .paint()
            .bounds("0")
            .fallback(&LineChartProps::line_width_default)
            .note("Line width (dp)");
    }

    /// @brief 可空值件：数据点半径（dp）；回落基线 3.0，下界 0。
    ///
    /// 本行还决定悬停命中阈值（半径 + 8dp），那是命中反查、不是第二个消费档位。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto dot_radius_row() {
        return prop_row<&LineChartProps::dot_radius_>("dot_radius")
            .paint()
            .bounds("0")
            .fallback(&LineChartProps::dot_radius_default)
            .note("Data point radius (dp)");
    }

    /// @brief 可空值件：悬停十字准线开关（吸附最近数据 x）；回落基线为开。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_crosshair_row() {
        return prop_row<&LineChartProps::show_crosshair_>("show_crosshair")
            .paint()
            .fallback(&LineChartProps::show_crosshair_default)
            .note("Draw a crosshair snapped to the nearest data point on hover");
    }

    /// @brief 可空值件：类目轴规格。回落点是默认构造的 `ChartAxisSpec`（可见、五刻度、画网格）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_x_row() {
        return prop_row<&LineChartProps::axis_x_>("axis_x")
            .layout()
            .paint()
            .fallback(&LineChartProps::axis_x_default)
            .note("Category axis spec: {visible,label,tick_count,show_grid_lines}");
    }

    /// @brief 可空值件：数值轴（Linear）规格。回落点是默认构造的 `ChartAxisSpec`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto axis_y_row() {
        return prop_row<&LineChartProps::axis_y_>("axis_y")
            .layout()
            .paint()
            .fallback(&LineChartProps::axis_y_default)
            .note("Value axis spec: {visible,label,tick_count,min,max,show_grid_lines,include_zero}");
    }

    /// @brief 可空值件：图例规格。回落点是默认构造的 `ChartLegendSpec`（可见、顶部）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto legend_row() {
        return prop_row<&LineChartProps::legend_>("legend")
            .layout()
            .paint()
            .fallback(&LineChartProps::legend_default)
            .note("Legend spec: {visible,position:Top|Bottom|Right}");
    }

    /// @brief 可空值件：图内留白（dp），轴标签 / 值框的避让区。回落基线四边各 8.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&LineChartProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&LineChartProps::padding_default)
            .note("In-chart padding (dp)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(series_row(), categories_row(), show_dots_row(), line_width_row(), dot_radius_row(),
                              show_crosshair_row(), axis_x_row(), axis_y_row(), legend_row(), padding_row());
    }

    // ---- 读面：每行恰一件，十行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的数据系列。
    /// @return 生效序列（数组行按值给出：未表态时为空序列）。
    [[nodiscard]] auto series() const -> std::vector<ChartSeries> { return stated(series_row()); }

    /// @brief 当前生效的 x 轴类目标签。
    /// @return 生效标签（未表态时为空序列；补齐到点位数是控件侧 `resolved_categories` 的事）。
    [[nodiscard]] auto categories() const -> std::vector<std::string> { return stated(categories_row()); }

    /// @brief 当前生效的数据点圆点开关。
    /// @return 生效开关。
    [[nodiscard]] auto show_dots() const -> bool { return stated(show_dots_row()); }

    /// @brief 当前生效的折线宽度（dp）。
    /// @return 生效线宽。
    [[nodiscard]] auto line_width() const -> float { return stated(line_width_row()); }

    /// @brief 当前生效的数据点半径（dp）。
    /// @return 生效半径。
    [[nodiscard]] auto dot_radius() const -> float { return stated(dot_radius_row()); }

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
    /// @param value 新系列（`values` 等距，x = 索引）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_series(const std::vector<ChartSeries> &value) -> LineChartProps & { return assign(series_row(), value); }

    /// @brief 装入 x 轴类目标签。
    /// @param value 类目标签列表；不足点位数处以序号补齐。
    /// @return 引用本属性对象，便于链式续写。
    auto set_categories(const std::vector<std::string> &value) -> LineChartProps & {
        return assign(categories_row(), value);
    }

    /// @brief 装入数据点圆点开关。
    /// @param value true 时在数据点绘制圆点。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_dots(bool value) -> LineChartProps & { return assign(show_dots_row(), value); }

    /// @brief 装入折线宽度（dp）。
    /// @param value 新线宽；0 是「显式要一条不画出来的线」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_line_width(float value) -> LineChartProps & { return assign(line_width_row(), value); }

    /// @brief 装入数据点半径（dp）；同时决定悬停命中阈值（半径 + 8dp）。
    /// @param value 新半径。
    /// @return 引用本属性对象，便于链式续写。
    auto set_dot_radius(float value) -> LineChartProps & { return assign(dot_radius_row(), value); }

    /// @brief 装入十字准线开关。
    /// @param value true 时悬停绘制吸附准线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_crosshair(bool value) -> LineChartProps & { return assign(show_crosshair_row(), value); }

    /// @brief 装入类目轴规格。
    /// @param value 轴规格（可见性 / 标签 / 刻度数等）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_x(const ChartAxisSpec &value) -> LineChartProps & { return assign(axis_x_row(), value); }

    /// @brief 装入数值轴（Linear）规格。
    /// @param value 轴规格（可见性 / 标签 / 刻度数 / 显式域 / include_zero 等）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_axis_y(const ChartAxisSpec &value) -> LineChartProps & { return assign(axis_y_row(), value); }

    /// @brief 装入图例规格。
    /// @param value 图例规格（可见性 / 位置）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_legend(const ChartLegendSpec &value) -> LineChartProps & { return assign(legend_row(), value); }

    /// @brief 装入图内留白（dp）。
    /// @param value 四边留白。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> LineChartProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 数据系列回落基线。
    /// @return 空序列（无数据 ⇒ 不画）。
    [[nodiscard]] static auto series_default() -> std::vector<ChartSeries> { return {}; }

    /// @brief 类目标签回落基线。
    /// @return 空序列（⇒ 全部类目以序号补齐）。
    [[nodiscard]] static auto categories_default() -> std::vector<std::string> { return {}; }

    /// @brief 数据点圆点开关回落基线。
    /// @return true（默认画数据点圆点）。
    [[nodiscard]] static auto show_dots_default() -> bool { return true; }

    /// @brief 折线宽度回落基线。
    /// @return 2.0F（dp）。
    [[nodiscard]] static auto line_width_default() -> float { return 2.0F; }

    /// @brief 数据点半径回落基线。
    /// @return 3.0F（dp）。
    [[nodiscard]] static auto dot_radius_default() -> float { return 3.0F; }

    /// @brief 十字准线开关回落基线。
    /// @return true（默认画准线）。
    [[nodiscard]] static auto show_crosshair_default() -> bool { return true; }

    /// @brief 类目轴回落基线。
    /// @return 默认构造的轴规格（可见、五刻度、画网格、域含 0）。
    [[nodiscard]] static auto axis_x_default() -> ChartAxisSpec { return {}; }

    /// @brief 数值轴回落基线。
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
    std::optional<std::vector<ChartSeries>> series_;  ///< 数据系列表态位
    std::optional<std::vector<std::string>> categories_;  ///< 类目标签表态位
    std::optional<bool> show_dots_;  ///< 数据点圆点开关表态位
    std::optional<float> line_width_;  ///< 折线宽度表态位（dp）
    std::optional<float> dot_radius_;  ///< 数据点半径表态位（dp）
    std::optional<bool> show_crosshair_;  ///< 十字准线开关表态位
    std::optional<ChartAxisSpec> axis_x_;  ///< 类目轴规格表态位
    std::optional<ChartAxisSpec> axis_y_;  ///< 数值轴规格表态位
    std::optional<ChartLegendSpec> legend_;  ///< 图例规格表态位
    std::optional<EdgeInsets> padding_;  ///< 图内留白表态位（dp）

    friend class AbstractProps<LineChartProps>;
};

/// @brief 折线图控件（叶控件；契约见 specification/04-widget.md §3.8）。
///
/// 折线经 Painter 的 `stroke_polyline`（真 SDF，圆角连接），数据点用圆角矩形复用圆形。
/// x 为等距索引（无独立 x 域），y 域与命中反查同源于 `LinearScale`。
/// 悬停按「最近数据点欧氏距离」命中（阈值 = `dot_radius + 8dp`），并叠加吸附该点的十字准线。
///
/// 属性一律经 `LineChartProps` 的读写件访问（本类公有继承它，故 `chart.set_line_width(3.0F)` 与
/// `chart.line_width()` 都直接可用）；十行皆常量档，读件一律无参。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class LineChart : public LeafWidget, public LineChartProps {
  public:
    /// @brief 默认构造：十行皆未表态（生效值即各行回落基线）；并接线宿主。
    LineChart() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 折线图属性对象，移动进本实例的 `LineChartProps` 基类子对象。
    explicit LineChart(LineChartProps props) : LineChartProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `LineChartProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> LineChartProps { return {}; }

    /// @brief 点击（抬起）命中数据点时触发：`(系列索引, 点索引)`。旁挂，不进序列化面。
    /// @param series_idx 回调形参：命中的数据系列索引。
    /// @param point_idx 回调形参：命中的数据点索引。
    /// @return 回调对象本身；未挂载回调时为空 `std::function`。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int series_idx, int point_idx)> on_point_tapped;

    /// @brief 装入数据系列：转调属性面写件，再重播 grow-in 入场动画。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的写件：动画重播是控件自身的事实，不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值仍由同一行存储与回落给出）。
    /// @param value 新系列（`values` 等距，x = 索引）。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_series(const std::vector<ChartSeries> &value) -> LineChart & {
        LineChartProps::set_series(value);
        grow_.replay();  // 重播 grow-in 入场动画
        return *this;
    }

    /// @brief 类型名。
    /// @return 固定为 "LineChart"。
    [[nodiscard]] auto type_name() const -> const char * override { return "LineChart"; }

    /// @brief 静态描述符入口：属性矩阵由权威表派生，事件 / 不变式 / 样例留在本处。
    /// @return WidgetDescriptor（含 line_width ≥ 0、dot_radius ≥ 0 等不变式）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("LineChart");
        descriptor.events = {"on_point_tapped"};
        descriptor.children_policy = "none";
        descriptor.invariants = {"line_width >= 0", "dot_radius >= 0"};
        descriptor.examples = {
            "au::LineChart(au::LineChartProps().set_series({{ .name = \"A\", .values = {1, 3, 2} }}))"};
        return descriptor;
    }
    /// @brief 返回控件描述符，与 describe_static 同源。
    /// @return 同 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集可观察信号：登记各行已绑定的外部来源，再登记 grow-in 入场动画信号。
    /// @param out 收集输出容器，追加来源视图与动画信号裸指针。
    auto collect_signals(StateSink &out) -> void override {
        collect_prop_sources(out);
        out.push(grow_.signal());
    }

    /// @brief 动画期间不缓存 Display List（内容每帧变化）。
    /// @return 未在动画中时为 true（可缓存）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return !grow_.animating(); }

    /// @brief 序列化自有属性到 `props`：先由基类写通用属性，再写各行；未表态的行不写键。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 已登记校验的行（线宽 / 半径越界）非法值一律不写入并撤表态；系列 / 类目 / 轴 / 图例 /
    ///       留白按既有宽容口径逐项回退默认（不登记校验，故照常表态且不上报）。
    /// @param props 源 JSON 对象，按权威表逐行取键。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 图表自带点击目标语义：始终参与点击（数据点命中即触发回调）。
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
    ///        Release 命中数据点触发 on_point_tapped。
    /// @param e 鼠标事件；Move / Release 被消费（置 `is_handled`），其余回落基类。
    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`—— 推断表不识 `LineChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @return 统一为 `AccessibilityRole::Image`。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    /// @brief 无障碍标签：图类型与规模（数据本身不进语义树，避免读屏念出一长串数字）。
    /// @return 形如 "LineChart, N series, M points"。
    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值：当前悬停 / 选中的数据点（未悬停时为空）。
    /// @return 悬停点的 "系列名: 数值" 文本；未悬停或索引越界为空串。
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的数据点（系列索引, 点索引）；无悬停为空。
    /// @return 悬停点 (系列索引, 点索引)；未命中为 `std::nullopt`。
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
    /// @brief 布局期算定的绘图几何（局部坐标）：渲染与命中反查共用同一份。
    struct Geometry {
        Rect plot{};  ///< 折线绘制区（不含轴留白）
        LinearScale y_scale;  ///< 数值轴
        std::vector<std::string> cats;  ///< 解析后的类目标签（缺省以序号补齐）
        std::size_t point_count = 0;  ///< 数据点位数 = 类目数
        std::vector<Rect> legend_rects;  ///< 图例项命中区（局部坐标）
    };

    /// @brief 点位数 = max(类目标签数, 各系列 values 的最大长度)。
    /// @param data 本趟取出的数据系列（数组行按值读出，故由调用方取一次再传下来）。
    /// @param labels 本趟取出的类目标签。
    /// @return 数据点位数。
    [[nodiscard]] static auto point_count(const std::vector<ChartSeries> &data, const std::vector<std::string> &labels)
        -> std::size_t;
    /// @brief 解析生效类目标签：显式标签不足处以序号字符串 "1","2",… 补齐到点位数。
    /// @param data 本趟取出的数据系列。
    /// @param labels 本趟取出的类目标签。
    /// @return 与点位数等长的标签列表。
    [[nodiscard]] static auto resolved_categories(const std::vector<ChartSeries> &data,
                                                  const std::vector<std::string> &labels) -> std::vector<std::string>;
    /// @brief 计算数值轴域：全体值取域（非有限值剔除），空数据退化 [0,1]，axis_y.min/max 显式覆盖，
    ///        include_zero 并入 0。
    /// @param data 本趟取出的数据系列。
    /// @return 带刻度文本的 LinearScale。
    [[nodiscard]] auto compute_y_scale(const std::vector<ChartSeries> &data) const -> LinearScale;
    /// @brief 由尺寸与字体算定绘图几何：扣除图例带与轴标签留白得绘图区，并生成图例命中区。
    /// @param size 控件尺寸。
    /// @param font 继承主题的字体。
    /// @return 渲染与命中反查共用的 Geometry（同源）。
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    /// @brief 第 i 个数据点的 x 坐标（等距：单点居中，否则沿绘图区线性均分；x = 索引，无独立 x 域）。
    /// @param g 布局期几何。
    /// @param i 点索引。
    /// @return 像素位置（局部坐标）。
    [[nodiscard]] static auto point_x(const Geometry &g, std::size_t i) -> float;
    /// @brief 反查命中的图例项（命中区与绘制同源于 geom_.legend_rects）。
    /// @param g 布局期几何。
    /// @param local 控件局部坐标。
    /// @return 图例项索引；未命中为 `std::nullopt`。
    [[nodiscard]] static auto legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t>;
    /// @brief 最近数据点命中：遍历全部系列求欧氏距离，阈值 = dot_radius + 8dp。
    /// @param g 布局期几何。
    /// @param local 控件局部坐标。
    /// @return 最近点 (系列索引, 点索引)；超阈值、无数据或未成图时为 `std::nullopt`。
    [[nodiscard]] auto nearest_point(const Geometry &g, const Point &local) const -> std::optional<std::pair<int, int>>;
    /// @brief 安全取数据点值：系列 / 点越界或非有限值（NaN / Inf）回退 0.0。
    /// @param data 本趟取出的数据系列（数组行按值读出，热点循环里绝不逐点重取）。
    /// @param series_idx 系列索引。
    /// @param point_idx 点索引。
    /// @return 数据点值。
    [[nodiscard]] static auto series_value(const std::vector<ChartSeries> &data, std::size_t series_idx,
                                           std::size_t point_idx) -> double;

    Geometry geom_;  ///< 布局期算定的绘图几何缓存（on_layout 写入，绘制 / 命中反查复用）
    ChartGrowIn grow_;  ///< grow-in 入场动画状态（描线进度 0→1；on_mount 接入帧循环）
    std::optional<std::pair<int, int>> hovered_point_;  ///< 当前悬停的数据点（系列, 点）；无悬停为空
    std::optional<std::size_t> legend_hover_;  ///< 当前悬停的图例项索引；无悬停为空
};

inline auto LineChart::point_count(const std::vector<ChartSeries> &data, const std::vector<std::string> &labels)
    -> std::size_t {
    std::size_t n = labels.size();
    for (const ChartSeries &s : data) {
        n = std::max(n, s.values.size());
    }
    return n;
}

inline auto LineChart::resolved_categories(const std::vector<ChartSeries> &data, const std::vector<std::string> &labels)
    -> std::vector<std::string> {
    const std::size_t n = point_count(data, labels);
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(i < labels.size() ? labels[i] : std::to_string(i + 1));
    }
    return out;
}

inline auto LineChart::series_value(const std::vector<ChartSeries> &data, std::size_t series_idx, std::size_t point_idx)
    -> double {
    if (series_idx >= data.size()) {
        return 0.0;
    }
    const std::vector<double> &vals = data[series_idx].values;
    if (point_idx >= vals.size()) {
        return 0.0;
    }
    return std::isfinite(vals[point_idx]) ? vals[point_idx] : 0.0;
}

// min 与 max 同时显式时走 from_explicit，否则 from_domain；空数据退化 [0,1]。
inline auto LineChart::compute_y_scale(const std::vector<ChartSeries> &data) const -> LinearScale {
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
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
    if (!have) {
        lo = 0.0;
        hi = 1.0;
    }
    const ChartAxisSpec axis = axis_y();  // 结构化行按值读出，本趟只取一次
    if (axis.min.has_value()) {
        lo = *axis.min;
    }
    if (axis.max.has_value()) {
        hi = *axis.max;
    }
    if (axis.include_zero) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    if (axis.min.has_value() && axis.max.has_value()) {
        return LinearScale::from_explicit(lo, hi, axis.tick_count);
    }
    return LinearScale::from_domain(lo, hi, axis.tick_count);
}

inline auto LineChart::point_x(const Geometry &g, std::size_t i) -> float {
    if (g.point_count <= 1U) {
        return g.plot.origin.x + (g.plot.size.width * 0.5F);
    }
    const float t = static_cast<float>(i) / static_cast<float>(g.point_count - 1U);
    return g.plot.origin.x + (t * g.plot.size.width);
}

// 轴可见性与刻度文本宽度决定左右 / 底部预留；图例命中区按与绘制同一套游标规则生成。
inline auto LineChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    // 五行在本函数里逐档多次消费，数组行与结构化行皆按值读出 ⇒ 本趟各取一次再复用。
    const std::vector<ChartSeries> data = series();
    const std::vector<std::string> labels = categories();
    const EdgeInsets pad = padding();
    const ChartLegendSpec leg = legend();
    const ChartAxisSpec x_axis = axis_x();
    const ChartAxisSpec y_axis = axis_y();

    Geometry g;
    g.cats = resolved_categories(data, labels);
    g.point_count = g.cats.size();
    g.y_scale = compute_y_scale(data);

    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    float left = pad.left;
    float top = pad.top;
    float right = std::max(pad.left, size.width - pad.right);
    float bottom = std::max(pad.top, size.height - pad.bottom);

    // 图例带 + 命中区（图例项区域优先于数据区）
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

    if (x_axis.visible) {
        bottom -= line_h;
        if (!x_axis.label.empty()) {
            bottom -= line_h;
        }
    }
    if (y_axis.visible) {
        float widest = 0.0F;
        const int digits = tick_digits(g.y_scale.step());
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

    // 图例项命中区（与绘制同一套游标推进规则）
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

/// @brief 布局：填充最大约束（无界时退化 300×200 缺省画布），非负化后按主题字体算定并缓存 geom_。
/// @param c 来自父级的约束。
/// @param ctx 构建上下文（继承字体参与几何预留）。
/// @return 采用的控件尺寸。
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
    geom_ = compute_geometry(s, inherit_theme(ctx).type_scale.body_medium);
    return s;
}

// 对 legend_rects 逐项做矩形包含判断。
inline auto LineChart::legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t> {
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
    // 命中反查逐点遍历全部系列：数组行按值读出，故本趟只取一次。
    const std::vector<ChartSeries> data = series();
    if (g.plot.size.width <= 0.0F || g.plot.size.height <= 0.0F || g.point_count == 0U || data.empty()) {
        return std::nullopt;
    }
    const float threshold = dot_radius() + 8.0F;
    float best = threshold;
    std::optional<std::pair<int, int>> best_hit;
    for (std::size_t k = 0; k < data.size(); ++k) {
        for (std::size_t i = 0; i < g.point_count; ++i) {
            const float px = point_x(g, i);
            const float py = g.y_scale.to_px(series_value(data, k, i), g.plot.bottom(), g.plot.origin.y);
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
    const std::vector<ChartSeries> data = series();
    const std::vector<std::string> labels = categories();
    return "LineChart, " + std::to_string(data.size()) + " series, " + std::to_string(point_count(data, labels)) +
           " points";
}

inline auto LineChart::accessibility_value() const -> std::string {
    if (!hovered_point_.has_value()) {
        return std::string{};
    }
    const std::vector<ChartSeries> data = series();
    const auto [k, i] = *hovered_point_;
    if (k < 0 || i < 0 || static_cast<std::size_t>(k) >= data.size()) {
        return std::string{};
    }
    return data[static_cast<std::size_t>(k)].name + ": " +
           std::to_string(series_value(data, static_cast<std::size_t>(k), static_cast<std::size_t>(i)));
}

/// @brief 绘制：数值轴、折线（grow-in 描线进度截断 + 末段插值）、数据点圆点（图例联动降透明）、
///        类目轴、图例（与 geom_.legend_rects 同源）、十字准线 / 悬停高亮环 / 自绘值框（夹进 bounds）。
/// @param p 软件绘制器。
/// @param bounds 控件全局矩形（局部几何经原点偏移换算为全局坐标）。
/// @param ctx 构建上下文（继承主题字体 / 配色）。
inline auto LineChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme &theme = inherit_theme(ctx);
    const Font &font = theme.type_scale.body_medium;
    const Point origin = bounds.origin;
    const Geometry &g = geom_;
    const Rect &plot = g.plot;
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 31};
    const Color axis = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 160};

    // 绘制档各行本帧只读一次：数组行与结构化行皆按值给出，逐点循环里绝不重取。
    const std::vector<ChartSeries> data = series();
    const ChartAxisSpec x_axis = axis_x();
    const ChartAxisSpec y_axis = axis_y();
    const ChartLegendSpec leg = legend();
    const bool dots = show_dots();
    const float stroke = line_width();
    const float radius = dot_radius();
    const bool crosshair = show_crosshair();

    if (y_axis.visible) {
        draw_numeric_axis(p, origin, plot, g.y_scale, y_axis, font, grid, axis);
    }

    // ---- 折线 + 数据点（含图例联动降透明）----
    const double grow_t = grow_.progress();  // grow-in：描线进度（无 Animator 时恒 1）
    std::optional<Point> hovered_px;
    for (std::size_t k = 0; k < data.size(); ++k) {
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
        Color c = resolve_series_color(k, data[k].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        std::vector<Point> pts;
        pts.reserve(g.point_count);
        for (std::size_t i = 0; i < g.point_count; ++i) {
            const float px = point_x(g, i);
            const float py = g.y_scale.to_px(series_value(data, k, i), plot.bottom(), plot.origin.y);
            pts.push_back(Point{.x = origin.x + px, .y = origin.y + py});
            if (hovered_point_.has_value() && std::cmp_equal(hovered_point_->first, k) &&
                std::cmp_equal(hovered_point_->second, i)) {
                hovered_px = Point{.x = px, .y = py};
            }
        }
        // grow-in：只绘制到 t 对应的折线前缀（末段按分数插值端点）
        std::vector<Point> drawn = pts;
        std::size_t dot_count = pts.size();
        if (grow_t < 1.0 && pts.size() >= 2) {
            const float span = static_cast<float>(pts.size() - 1) * static_cast<float>(grow_t);
            const auto whole = static_cast<std::size_t>(std::floor(span));
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
        if (drawn.size() >= 2 && stroke > 0.0F) {
            p.stroke_polyline(drawn, stroke, c);
        }
        if (dots && radius > 0.0F) {
            const float d = radius * 2.0F;
            for (std::size_t idx = 0; idx < dot_count && idx < pts.size(); ++idx) {
                const Point &pt = pts[idx];
                const Rect dot{.origin = Point{.x = pt.x - radius, .y = pt.y - radius},
                               .size = Size{.width = d, .height = d}};
                p.fill_rounded_rect(dot, radius, c);
            }
        }
    }

    // ---- 类目轴 ----
    if (x_axis.visible) {
        BandScale band{g.point_count};
        draw_category_axis(p, origin, plot, band, g.cats, x_axis, font, axis);
    }

    // ---- 图例 ----
    if (leg.visible) {
        for (std::size_t k = 0; k < g.legend_rects.size() && k < data.size(); ++k) {
            const Rect &r = g.legend_rects[k];
            const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != k);
            Color c = resolve_series_color(k, data[k].color, theme);
            if (dimmed) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = origin.x + r.origin.x, .y = origin.y + r.origin.y + 3.0F},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(swatch, 2.0F, c);
            const Rect box{.origin = Point{.x = origin.x + r.origin.x + 14.0F, .y = origin.y + r.origin.y},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(box, data[k].name, font, axis);
        }
    }

    // ---- 十字准线 + 悬停高亮 + 值框 ----
    if (hovered_px.has_value()) {
        if (crosshair) {
            p.draw_line(Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.origin.y},
                        Point{.x = origin.x + hovered_px->x, .y = origin.y + plot.bottom()}, 1.0F, grid);
        }
        const float d = std::max(4.0F, radius * 2.0F) + 4.0F;
        const Rect ring{
            .origin = Point{.x = origin.x + hovered_px->x - (d * 0.5F), .y = origin.y + hovered_px->y - (d * 0.5F)},
            .size = Size{.width = d, .height = d}};
        p.draw_rounded_border(ring, d * 0.5F, 2.0F, theme.roles.on_surface);

        if (hovered_point_.has_value()) {
            const auto [k, i] = *hovered_point_;
            if (k >= 0 && static_cast<std::size_t>(k) < data.size()) {
                const std::string text =
                    data[static_cast<std::size_t>(k)].name + ": " +
                    std::to_string(series_value(data, static_cast<std::size_t>(k), static_cast<std::size_t>(i)));
                const float w = render::FontEngine::measure_width(text, font) + 16.0F;
                const float h = line_h + 8.0F;
                float bx = hovered_px->x + 8.0F;
                float by = hovered_px->y - h - 4.0F;
                bx = std::clamp(bx, 0.0F, std::max(0.0F, bounds.size.width - w));
                by = std::clamp(by, 0.0F, std::max(0.0F, bounds.size.height - h));
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
