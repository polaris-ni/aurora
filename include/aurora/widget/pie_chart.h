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

/// @brief PieChart 的属性权威表：一行一字段，键名、私有字段、脏档位、约束与回落点只在行里声明一次。
///
/// 读件、写件、序列化、订阅采集与 schema 描述全部由这张表派生，因此本类不再有可直读的公有字段。
/// 七行皆为**可空值件 + 常量档回落点**：饼图的配色取自继承主题与色板、不属属性面，故七枚读件一律
/// 无参；「未设」由表态位表达，不再外泄为 `std::optional` 字段。
///
/// 迁移前七行都是带声明处默认值的裸字段；现在声明处默认值逐个落成各行回落基线（`sections` 的基线是
/// 空序列、`legend` 的基线是默认构造的 `ChartLegendSpec`），生效值逐值不变。
///
/// 档位按「哪几趟消费本行」给出，与迁移前逐枚 setter 手写的标脏口径有四行不同，其中两行是行为修正：
/// `center_space_ratio` 与 `legend` 迁移前只标绘制 / 只标布局各半，但两者都被布局期的 `compute_geometry`
/// **和**绘制趟直接读（内半径在那趟算定、图例可见性与名称在两趟都要），单标一档会让另一趟停在旧值。
/// 另两行只是口径归位：`padding` 迁移前只标布局，此处随图表族同键口径补绘制档（本型绘制趟确实直接读
/// `legend` 与 `sections`，留白则经由缓存的几何参与）；`section_gap` 迁移前**没有写件**（只能经字段直写
/// 或 JSON 抵达），本表按「每行恰一写件」给出写件并只标绘制档。余下三行（`sections` / `start_angle` /
/// `show_percentage_labels`）迁移前后同档（`start_angle` 还被命中反查读，那是交互趟、不属脏档位）。
///
/// 两枚结构化 / 数组行与两枚数值行走的是两条不同的校验通路。`sections` 与 `legend` 的编解码登记在
/// `chart_common.h` 里：类型名、JSON 类型与双向转换都由登记给出，本表只点名它们，而它们一律**未登记
/// `validate`** —— 迁移前那两条通路就是「字段缺失 / 类型不符逐项回退默认」的宽容口径，故这类行非法值
/// 不撤销表态、也不产生降级上报。两枚数值行（`center_space_ratio` / `section_gap`）则相反：`float` 登记
/// 自带按描述符约束的校验，故 JSON 里的越界值**不再夹取写入**，而是不写入并撤表态、上报降级。运行时写件
/// 里 `section_gap` 与迁移前同（本来就没有写件），`center_space_ratio` **不同**：旧写件在写入时就把值夹进
/// `[0,0.95]`，故迁移前 `set_center_space_ratio(2.0F)` 之后读回 `0.95F`；现写件照原样存入、读件回读原值，
/// 越界仍由 `compute_geometry` 在使用点夹取 ⇒ 生效像素逐字节不变，代价是「表上写了什么」与「画出来用
/// 什么」从此分列。`padding` 与开关两行也改了判：旧路径是 `json_to_edge_insets` 直接覆盖（非对象整值烘成
/// 四边全 0 且照常取值、负字段照收）与「非数值 / 非 bool 静默跳过保留现值」，现经 `PropCodec<EdgeInsets>`
/// 与 `bool` 登记的 `validate`，形态不符一律拒写、撤表态并上报。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class PieChartProps : public AbstractProps<PieChartProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：扇区列表；占比 = value / Σvalue。回落点是空序列。
    ///
    /// 档位取「布局 + 绘制」：占比与图例宽度在布局期算定，色块与名称在绘制趟直接读。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto sections_row() {
        return prop_row<&PieChartProps::sections_>("sections")
            .layout()
            .paint()
            .fallback(&PieChartProps::sections_default)
            .note("Sector array: [{name, value, color?}], share = value / sum(value)");
    }

    /// @brief 可空值件：内径 / 外径比（> 0 即环图）；回落基线 0.0，约束区间 [0,0.95]（几何计算再夹一次）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto center_space_ratio_row() {
        return prop_row<&PieChartProps::center_space_ratio_>("center_space_ratio")
            .layout()
            .paint()
            .bounds("0", "0.95")
            .fallback(&PieChartProps::center_space_ratio_default)
            .note("Inner / outer radius ratio; > 0 makes a donut");
    }

    /// @brief 可空值件：起始角（度，0 = +x 方向；-90 = 12 点钟）；回落基线 -90.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto start_angle_row() {
        return prop_row<&PieChartProps::start_angle_>("start_angle")
            .paint()
            .fallback(&PieChartProps::start_angle_default)
            .note("Start angle (degrees, 0 = +x direction; -90 = 12 o'clock)");
    }

    /// @brief 可空值件：扇区内百分比文本开关；回落基线为关。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto show_percentage_labels_row() {
        return prop_row<&PieChartProps::show_percentage_labels_>("show_percentage_labels")
            .paint()
            .fallback(&PieChartProps::show_percentage_labels_default)
            .note("Percentage text inside the sectors");
    }

    /// @brief 可空值件：扇区间隙（dp，按角度换算后两侧各让出一半）；回落基线 2.0，下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto section_gap_row() {
        return prop_row<&PieChartProps::section_gap_>("section_gap")
            .paint()
            .bounds("0")
            .fallback(&PieChartProps::section_gap_default)
            .note("Gap between sectors (dp), yielded half on each side after the angle conversion");
    }

    /// @brief 可空值件：图例规格。回落点是默认构造的 `ChartLegendSpec`（可见、顶部）。
    ///
    /// 本行同时进布局（图例带扣减绘图区）与绘制（色块与名称），故两档齐标。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto legend_row() {
        return prop_row<&PieChartProps::legend_>("legend")
            .layout()
            .paint()
            .fallback(&PieChartProps::legend_default)
            .note("Legend spec: {visible,position:Top|Bottom|Right}");
    }

    /// @brief 可空值件：图内留白（dp），圆心与图例带的避让区。回落基线四边各 8.0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto padding_row() {
        return prop_row<&PieChartProps::padding_>("padding")
            .layout()
            .paint()
            .fallback(&PieChartProps::padding_default)
            .note("In-chart padding (dp), reserved for the plot area and the legend band");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(sections_row(), center_space_ratio_row(), start_angle_row(), show_percentage_labels_row(),
                              section_gap_row(), legend_row(), padding_row());
    }

    // ---- 读面：每行恰一件，七行皆常量档 ⇒ 全部无参 ----

    /// @brief 当前生效的扇区列表。
    /// @return 生效序列（数组行按值给出：未表态时为空序列）。
    [[nodiscard]] auto sections() const -> std::vector<PieSection> { return stated(sections_row()); }

    /// @brief 当前生效的内径 / 外径比。
    /// @return 生效比值（几何计算时夹取 [0,0.95]）。
    [[nodiscard]] auto center_space_ratio() const -> float { return stated(center_space_ratio_row()); }

    /// @brief 当前生效的起始角（度）。
    /// @return 生效角度。
    [[nodiscard]] auto start_angle() const -> float { return stated(start_angle_row()); }

    /// @brief 当前生效的百分比文本开关。
    /// @return 生效开关。
    [[nodiscard]] auto show_percentage_labels() const -> bool { return stated(show_percentage_labels_row()); }

    /// @brief 当前生效的扇区间隙（dp）。
    /// @return 生效间隙。
    [[nodiscard]] auto section_gap() const -> float { return stated(section_gap_row()); }

    /// @brief 当前生效的图例规格。
    /// @return 生效规格（未表态时为可见、顶部）。
    [[nodiscard]] auto legend() const -> ChartLegendSpec { return stated(legend_row()); }

    /// @brief 当前生效的图内留白（dp）。
    /// @return 生效留白。
    [[nodiscard]] auto padding() const -> EdgeInsets { return stated(padding_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入扇区列表；表态即脱离空序列基线。
    /// @param value 新扇区列表（占比随 value / Σvalue 变化）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_sections(const std::vector<PieSection> &value) -> PieChartProps & { return assign(sections_row(), value); }

    /// @brief 装入内径 / 外径比。
    /// @param value 新比值；越界值运行时照存，由几何计算的夹取兜底。
    /// @return 引用本属性对象，便于链式续写。
    auto set_center_space_ratio(float value) -> PieChartProps & { return assign(center_space_ratio_row(), value); }

    /// @brief 装入起始角。
    /// @param value 角度（0 = +x 方向；-90 = 12 点钟起）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_start_angle(float value) -> PieChartProps & { return assign(start_angle_row(), value); }

    /// @brief 装入扇区内百分比文本开关。
    /// @param value 是否绘制百分比文本。
    /// @return 引用本属性对象，便于链式续写。
    auto set_show_percentage_labels(bool value) -> PieChartProps & {
        return assign(show_percentage_labels_row(), value);
    }

    /// @brief 装入扇区间隙（dp）。
    /// @param value 新间隙；负值照存，绘制趟的角度换算把它当 0 处理。
    /// @return 引用本属性对象，便于链式续写。
    auto set_section_gap(float value) -> PieChartProps & { return assign(section_gap_row(), value); }

    /// @brief 装入图例规格。
    /// @param value 图例规格（可见性 / 位置）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_legend(const ChartLegendSpec &value) -> PieChartProps & { return assign(legend_row(), value); }

    /// @brief 装入图内留白（dp）。
    /// @param value 四边留白。
    /// @return 引用本属性对象，便于链式续写。
    auto set_padding(EdgeInsets value) -> PieChartProps & { return assign(padding_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 扇区列表回落基线。
    /// @return 空序列（无扇区 ⇒ 绘制早返回）。
    [[nodiscard]] static auto sections_default() -> std::vector<PieSection> { return {}; }

    /// @brief 内径 / 外径比回落基线。
    /// @return 0.0F（实心饼图形态）。
    [[nodiscard]] static auto center_space_ratio_default() -> float { return 0.0F; }

    /// @brief 起始角回落基线。
    /// @return -90.0F（12 点钟起）。
    [[nodiscard]] static auto start_angle_default() -> float { return -90.0F; }

    /// @brief 百分比文本开关回落基线。
    /// @return false（默认不画扇区内文本）。
    [[nodiscard]] static auto show_percentage_labels_default() -> bool { return false; }

    /// @brief 扇区间隙回落基线。
    /// @return 2.0F（dp）。
    [[nodiscard]] static auto section_gap_default() -> float { return 2.0F; }

    /// @brief 图例回落基线。
    /// @return 默认构造的图例规格（可见、顶部）。
    [[nodiscard]] static auto legend_default() -> ChartLegendSpec { return {}; }

    /// @brief 图内留白回落基线。
    /// @return 四边各 8.0dp。
    [[nodiscard]] static auto padding_default() -> EdgeInsets {
        return EdgeInsets{.left = 8.0F, .top = 8.0F, .right = 8.0F, .bottom = 8.0F};
    }

  private:
    std::optional<std::vector<PieSection>> sections_;  ///< 扇区列表表态位
    std::optional<float> center_space_ratio_;  ///< 内径 / 外径比表态位
    std::optional<float> start_angle_;  ///< 起始角表态位（度）
    std::optional<bool> show_percentage_labels_;  ///< 百分比文本开关表态位
    std::optional<float> section_gap_;  ///< 扇区间隙表态位（dp）
    std::optional<ChartLegendSpec> legend_;  ///< 图例规格表态位
    std::optional<EdgeInsets> padding_;  ///< 图内留白表态位（dp）

    friend class AbstractProps<PieChartProps>;
};

/// @brief 饼图 / 环图控件（叶控件；契约见 specification/04-widget.md §3.8）。
///
/// 扇区经 `fill_sector` 绘制（`center_space_ratio > 0` 即环图），间隙以角度让位实现（非描边）。
/// 命中使用**极坐标**判定：先按半径落在 `[inner, outer]`，再按角度定位扇区（与渲染同源）。
///
/// 属性一律经 `PieChartProps` 的读写件访问（本类公有继承它，故 `chart.set_section_gap(4.0F)` 与
/// `chart.center_space_ratio()` 都直接可用）；七行皆常量档，读件一律无参。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class PieChart : public LeafWidget, public PieChartProps {
  public:
    /// @brief 默认构造：七行皆未表态（生效值即各行回落基线）；并接线宿主。
    PieChart() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 饼图属性对象，移动进本实例的 `PieChartProps` 基类子对象。
    explicit PieChart(PieChartProps props) : PieChartProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `PieChartProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> PieChartProps { return {}; }

    /// @brief 点击（抬起）命中扇区时触发：`(扇区索引)`。旁挂，不进序列化面。
    /// @param section_idx 回调收到的扇区索引（0 基）。
    /// @return 回调函数对象；缺省未挂接（空 std::function）。
    /// NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::function<void(int section_idx)> on_section_tapped;

    /// @brief 装入扇区列表：转调属性面写件，再重播 grow-in 入场动画。
    ///
    /// 本控件唯一需要「写入之后另做一件事」的写件：动画重播是控件自身的事实，不进属性表，
    /// 故控件侧留一枚同名覆写、按名隐藏属性面那件（不是第二个答案：值仍由同一行存储与回落给出）。
    /// @param value 新扇区列表（占比随 value / Σvalue 变化）。
    /// @return 引用本控件，便于链式续写。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名覆写是迁移前的既有契约
    auto set_sections(const std::vector<PieSection> &value) -> PieChart & {
        PieChartProps::set_sections(value);
        grow_.replay();  // 重播 grow-in 入场动画（未接帧循环时为 no-op）
        return *this;
    }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "PieChart"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "PieChart"; }

    /// @brief 静态自描述表：属性矩阵由权威表派生，事件 / 不变式 / 样例留在本处。
    /// @return WidgetDescriptor，含 width/height/show 公共属性与 on_section_tapped 事件。
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
    /// @note 已登记校验的行（`center_space_ratio` 越出 [0,0.95] / `section_gap` 为负 / `padding` 非对象或字段
    ///        非数值或为负 / `start_angle` 与 `show_percentage_labels` 形态不符）非法值一律不写入并撤表态；
    ///        扇区与图例按既有宽容口径逐项回退默认（不登记校验，故照常表态且不上报）。
    /// @param props 源 JSON 对象，按权威表逐行取键。
    auto deserialize_props(const Json &props) -> void override;

    /// @brief 声明接收点击事件（扇区轻触回调依赖）。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 悬停态变化：离开时清空扇区/图例悬停并重绘。
    /// @param entered 指针进入为 true，离开为 false。
    auto on_hover_change(bool entered) -> void override {
        hover_ = entered;
        if (!entered && (hovered_section_.has_value() || legend_hover_.has_value())) {
            hovered_section_.reset();
            legend_hover_.reset();
            mark_needs_paint();
        }
    }

    /// @brief 鼠标事件：Move 先图例后扇区更新悬停态（仅状态变化才重绘）；Release 命中扇区且回调已挂则触发
    ///        on_section_tapped。
    /// @param e 指针事件。
    auto on_pointer_event(MouseEvent &e) -> void override;

    /// @brief 无障碍角色：图表族统一为 `Image`—— 推断表不识 `PieChart`，
    ///        不覆写会回落 `Generic`，读屏念不出「这是一张图表」。
    /// @return 恒为 AccessibilityRole::Image。
    /// @note Side-effects: pure
    [[nodiscard]] auto accessibility_role() const -> AccessibilityRole override { return AccessibilityRole::Image; }

    /// @brief 无障碍标签：控件名与扇区数。
    /// @return "PieChart, N sections" 文本。
    [[nodiscard]] auto accessibility_label() const -> std::string override;
    /// @brief 无障碍值文本：悬停扇区的名称与原始值。
    /// @return 悬停且索引合法时为 "name: value"，无悬停或索引越界为空串。
    [[nodiscard]] auto accessibility_value() const -> std::string override;

    /// @brief 当前悬停的扇区索引；无悬停为空。
    /// @return hovered_section_ 的快照（nullopt = 无悬停）。
    [[nodiscard]] auto hovered_section() const -> std::optional<int> { return hovered_section_; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    /// @brief 绘制扇区、图例与悬停值框：扇区按 grow-in 进度展开总角、间隙以角度让位；图例悬停时非悬停扇区
    ///        减淡、悬停扇区描边、百分比文本夹在控件内；绘图区退化（外半径非正或无占比）时不绘制。
    /// @param p 绘制器
    /// @param bounds 绘制边界
    /// @param ctx 构建上下文（取主题色/字体）
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 接入帧循环并播放 grow-in（无运行中 Animator 时降级为终态）。
    /// @param ctx 构建上下文（本实现未使用）。
    auto on_mount([[maybe_unused]] const BuildContext &ctx) -> void override { grow_.mount(); }

  private:
    struct Geometry {
        Rect plot{};  ///< 绘图区矩形（留白与图例占位扣除后的可用区）
        Point center{};  ///< 饼/环图圆心（控件坐标）
        float outer = 0.0F;  ///< 外半径（绘图区宽高较小者之半）
        float inner = 0.0F;  ///< 内半径（外半径 × center_space_ratio，夹取 [0,0.95]）
        std::vector<double> ratios;  ///< 各扇区归一化占比（value / Σvalue）
        std::vector<Rect> legend_rects;  ///< 各图例项的命中矩形（与扇区序对齐）
    };

    /// @brief 依尺寸与字体计算几何（含按图例位置扣减绘图区与图例排布）。
    /// @param size 当前布局尺寸
    /// @param font 主题字体（量取图例文本宽高）
    /// @return Geometry 快照（on_layout 时存入 geom_）
    [[nodiscard]] auto compute_geometry(const Size &size, const Font &font) const -> Geometry;
    /// @brief 极坐标扇区命中判定（与渲染同源）。
    /// @param g 几何快照
    /// @param local 本地坐标
    /// @return 命中扇区索引；半径落环外/环内或角度无扇区时为 nullopt
    [[nodiscard]] auto section_hit(const Geometry &g, const Point &local) const -> std::optional<int>;
    /// @brief 图例项矩形命中判定。
    /// @param g 几何快照
    /// @param local 本地坐标
    /// @return 命中图例项序号，无命中为 nullopt
    [[nodiscard]] static auto legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t>;

    Geometry geom_;  ///< 当前几何（每次 on_layout 重算）
    ChartGrowIn grow_;  ///< grow-in 动画器（on_mount 接入帧循环）
    std::optional<int> hovered_section_;  ///< 当前悬停扇区（nullopt = 无）
    std::optional<std::size_t> legend_hover_;  ///< 当前悬停图例项（nullopt = 无）
};

// 绘图区先按留白内缩、再按图例位置扣掉一条图例带；圆心取绘图区中心，外半径取宽高较小者之半。
// 属性行按「每趟一份快照」读：本趟开头一次取齐扇区、留白、图例与中心比，避免同一趟里读到不一致的值。
inline auto PieChart::compute_geometry(const Size &size, const Font &font) const -> Geometry {
    Geometry g;
    const auto secs = sections();
    const auto pad = padding();
    const auto leg = legend();
    g.ratios = pie_section_ratios(secs);
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const float left = pad.left;
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
            for (const PieSection &s : secs) {
                widest = std::max(widest, render::FontEngine::measure_width(s.name, font));
            }
            right = std::max(left, right - (widest + 22.0F));
        }
    }
    g.plot = Rect{.origin = Point{.x = left, .y = top},
                  .size = Size{.width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)}};
    g.center =
        Point{.x = g.plot.origin.x + (g.plot.size.width * 0.5F), .y = g.plot.origin.y + (g.plot.size.height * 0.5F)};
    g.outer = std::min(g.plot.size.width, g.plot.size.height) * 0.5F;
    g.inner = g.outer * std::clamp(center_space_ratio(), 0.0F, 0.95F);

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
        for (const PieSection &s : secs) {
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

/// @brief 布局：按约束定尺寸（非有限约束回退 240×200），并重算 geom_。
/// @param c 父级约束
/// @param ctx 构建上下文（继承主题字体）
/// @return 本帧控件尺寸（宽高均夹到非负）
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
    geom_ = compute_geometry(s, inherit_theme(ctx).type_scale.body_medium);
    return s;
}

// 顺序扫描 legend_rects，首个包含该点的矩形即命中。
inline auto PieChart::legend_hit(const Geometry &g, const Point &local) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < g.legend_rects.size(); ++i) {
        const Rect &r = g.legend_rects[i];
        if (local.x >= r.origin.x && local.x <= r.right() && local.y >= r.origin.y && local.y <= r.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

// 半径先落在 [inner, outer]，再按相对 start_angle 归一化后的角扫描扇区。
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
    constexpr float two_pi = 6.28318530717958647692F;
    constexpr float deg = 0.01745329251994329577F;
    // 起始角偏移与绘制路径（`paint` 内的 `cursor`）同款取显式中间量，让未收缩形态下 12 点方向
    // 的角度精确为 0。但**不能依赖拆句防收缩**：优化器会把中间量前传回表达式再收缩（CI
    // ubuntu-26.04 的 gcc 实测如此——拆句后 simd-off 腿仍红）。收缩后 12 点方向残差约
    // 5.6e-8 弧度（单次舍入），故归一下方一律按「不信任输入」夹取，任何收缩开关下终点一致。
    const float start_offset = start_angle() * deg;
    float ang = std::atan2(dy, dx) - start_offset;
    ang = std::fmod(ang, two_pi);
    if (ang < 0.0F) {
        ang += two_pi;
    }
    // 夹回 [0, 2π)：残差为极小的负值时，`ang += two_pi` 的和会被舍入成**恰好 2π**（12 点方向
    // 的 -5.6e-8 即如此——ulp 4.8e-7 远大于残差本身），恰好落在下方扫描的严格 `<` 之外，整段
    // 丢命中（实测 toggles (simd-off) 三例：悬停 / 点击 / 交互分支全部命中不到扇区 0，而同一
    // 进程的 golden 像素比对照常通过——绘制路径不受影响）。减一次 2π 恢复不变量。
    if (ang >= two_pi) {
        ang -= two_pi;
    }
    // 扇区占比经 `pie_section_ratios` 归一化（Σ=1），数学上铺满整圆；最后一段**有效**扇区的
    // 上界因此取 2π 本身而非 `cursor + sweep`——浮点累加和可能略低于 2π，会把贴近 2π 的角度
    // 漏在最后一个扇区之外。零占比段被跳过、不占 cursor，「最后一段有效」按序取。
    std::size_t last_positive = 0;
    bool any_positive = false;
    for (std::size_t i = 0; i < g.ratios.size(); ++i) {
        if (static_cast<float>(g.ratios[i]) > 0.0F) {
            last_positive = i;
            any_positive = true;
        }
    }
    float cursor = 0.0F;
    for (std::size_t i = 0; i < g.ratios.size(); ++i) {
        const float sweep = static_cast<float>(g.ratios[i]) * two_pi;
        if (sweep <= 0.0F) {
            continue;
        }
        const float upper = (any_positive && i == last_positive) ? two_pi : cursor + sweep;
        if (ang >= cursor && ang < upper) {
            return static_cast<int>(i);
        }
        cursor += sweep;
    }
    return std::nullopt;
}

// Move/Release 以外的动作回落基类处理。
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

// 文本为控件名与生效扇区数的拼接（每趟一份快照）。
inline auto PieChart::accessibility_label() const -> std::string {
    return "PieChart, " + std::to_string(sections().size()) + " sections";
}

// 索引由悬停事件写入，读回时仍校验负值与越界；值取扇区的原始 value 而非占比。
inline auto PieChart::accessibility_value() const -> std::string {
    const auto secs = sections();
    if (!hovered_section_.has_value() || *hovered_section_ < 0 ||
        static_cast<std::size_t>(*hovered_section_) >= secs.size()) {
        return std::string{};
    }
    const auto i = static_cast<std::size_t>(*hovered_section_);
    return secs[i].name + ": " + std::to_string(secs[i].value);
}

// 属性矩阵由权威表派生（键名、类型、默认值、约束与说明逐行取自各行登记），事件 / 不变式 / 样例留在本处。
inline auto PieChart::describe_static() -> WidgetDescriptor {
    auto descriptor = props_descriptor("PieChart");
    descriptor.events = {"on_section_tapped"};
    descriptor.children_policy = "none";
    descriptor.invariants = {"0 <= center_space_ratio <= 0.95"};
    descriptor.examples = {
        "au::PieChart(au::PieChartProps().set_sections({{ .name = \"A\", .value = 3 }, "
        "{ .name = \"B\", .value = 1 }}))"};
    return descriptor;
}

// 逐行序列化：只写已表态的行，未表态的键不落盘（落盘的是意图，不是现值）。
inline auto PieChart::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props_serialize(props);
}

// 逐行反序列化：已登记校验的数值行越界即不写入并撤表态，结构化 / 数组行按宽容口径逐项回退默认。
inline auto PieChart::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    props_deserialize(props);
}

// 绘制顺序：扇区与扇内百分比标签 → 图例色块与名称 → 悬停值框（居中偏上并夹进控件边界）。
// 属性行按「每趟一份快照」读：间隙、起始角、扇区与图例各取一次，与 compute_geometry 同口径。
inline auto PieChart::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    const Theme &theme = inherit_theme(ctx);
    const Font &font = theme.type_scale.body_medium;
    const Point origin = bounds.origin;
    const Geometry &g = geom_;
    const auto secs = sections();
    const auto leg = legend();
    const float line_h = render::FontEngine::measure_height(font) + 4.0F;
    const Color grid = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 31};
    const Color axis = Color{theme.roles.on_surface.r, theme.roles.on_surface.g, theme.roles.on_surface.b, 160};

    constexpr float two_pi = 6.28318530717958647692F;
    constexpr float deg = 0.01745329251994329577F;
    if (g.outer <= 0.0F || g.ratios.empty()) {
        return;
    }
    // 间隙按外半径换算成角度，两侧各让出一半
    const float gap_rad = g.outer > 0.0F ? (section_gap() / g.outer) : 0.0F;
    const auto grow_t = static_cast<float>(grow_.progress());  // grow-in：扇形按 t 展开总角度
    float cursor = start_angle() * deg;
    for (std::size_t i = 0; i < g.ratios.size(); ++i) {
        const float sweep = static_cast<float>(g.ratios[i]) * two_pi * grow_t;
        if (sweep <= gap_rad) {
            cursor += sweep;
            continue;
        }
        const float a0 = cursor + (gap_rad * 0.5F);
        const float a1 = cursor + sweep - (gap_rad * 0.5F);
        const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != i);
        Color c = resolve_series_color(i, secs[i].color, theme);
        if (dimmed) {
            c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
        }
        p.fill_sector(Point{.x = origin.x + g.center.x, .y = origin.y + g.center.y}, g.outer, g.inner, a0, a1, c);
        if (hovered_section_.has_value() && std::cmp_equal(*hovered_section_, i)) {
            p.stroke_arc(Point{.x = origin.x + g.center.x, .y = origin.y + g.center.y}, (g.outer + g.inner) * 0.5F,
                         std::max(1.0F, g.outer - g.inner), a0, a1, theme.roles.on_surface);
        }
        if (show_percentage_labels() && g.ratios[i] > 0.0F) {
            const float mid = (a0 + a1) * 0.5F;
            const float r = (g.inner + g.outer) * 0.5F;
            const std::string text = format_number(g.ratios[i] * 100.0, Locale{}, 0) + "%";
            const float w = render::FontEngine::measure_width(text, font);
            const float tx = g.center.x + (std::cos(mid) * r) - (w * 0.5F);
            const float ty = g.center.y + (std::sin(mid) * r) - (line_h * 0.5F);
            // 标签一律夹在控件内
            const float cx = std::clamp(tx, 0.0F, std::max(0.0F, bounds.size.width - w));
            const float cy = std::clamp(ty, 0.0F, std::max(0.0F, bounds.size.height - line_h));
            const Rect box{.origin = Point{.x = origin.x + cx, .y = origin.y + cy},
                           .size = Size{.width = w + 2.0F, .height = line_h}};
            p.draw_text(box, text, font, axis);
        }
        cursor += sweep;
    }

    // ---- 图例 ----
    if (leg.visible) {
        for (std::size_t i = 0; i < g.legend_rects.size() && i < secs.size(); ++i) {
            const Rect &r = g.legend_rects[i];
            const bool dimmed = legend_hover_.has_value() && (*legend_hover_ != i);
            Color c = resolve_series_color(i, secs[i].color, theme);
            if (dimmed) {
                c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * 0.35F));
            }
            const Rect swatch{.origin = Point{.x = origin.x + r.origin.x, .y = origin.y + r.origin.y + 3.0F},
                              .size = Size{.width = 10.0F, .height = 10.0F}};
            p.fill_rounded_rect(swatch, 2.0F, c);
            const Rect box{.origin = Point{.x = origin.x + r.origin.x + 14.0F, .y = origin.y + r.origin.y},
                           .size = Size{.width = std::max(0.0F, r.size.width - 14.0F), .height = line_h}};
            p.draw_text(box, secs[i].name, font, axis);
        }
    }

    // ---- 悬停值框 ----
    if (hovered_section_.has_value() && *hovered_section_ >= 0 &&
        static_cast<std::size_t>(*hovered_section_) < secs.size()) {
        const auto i = static_cast<std::size_t>(*hovered_section_);
        const std::string text = secs[i].name + ": " + std::to_string(secs[i].value);
        const float w = render::FontEngine::measure_width(text, font) + 16.0F;
        const float h = line_h + 8.0F;
        const float bx = std::clamp(g.center.x + (g.outer * 0.5F), 0.0F, std::max(0.0F, bounds.size.width - w));
        const float by = std::clamp(g.center.y - g.outer - h - 4.0F, 0.0F, std::max(0.0F, bounds.size.height - h));
        const Rect box{.origin = Point{.x = origin.x + bx, .y = origin.y + by}, .size = Size{.width = w, .height = h}};
        p.fill_rounded_rect(box, 4.0F, Color{theme.roles.surface.r, theme.roles.surface.g, theme.roles.surface.b, 242});
        p.draw_rounded_border(box, 4.0F, 1.0F, grid);
        const Rect text_box{.origin = Point{.x = origin.x + bx + 8.0F, .y = origin.y + by + 4.0F},
                            .size = Size{.width = w - 16.0F, .height = line_h}};
        p.draw_text(text_box, text, font, theme.roles.on_surface);
    }
}

}  // namespace aurora