#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/core/directionality.h"
#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props.h"
#include "aurora/widget/widget.h"

/// @brief Aurora UI 库顶层命名空间（本头承载 Column / Row 线性容器与其布局上下文 trampoline）。
namespace aurora {

/// @brief 容器布局上下文：打包 children 指针 + 索引 + BuildContext 指针，供 trampoline 解包。
struct ContainerLayoutCtx : LayoutCtxBase {
    Node *children{};  ///< 子节点数组首元素
    size_t index = 0;  ///< 当前子项索引
    const BuildContext *build_ctx{};  ///< 构建上下文
};

/// @brief ContainerLayoutCtx 的 trampoline：void* → 具体类型 → 调用 widget.layout。
/// @param ctx 指向 `ContainerLayoutCtx` 的不透明指针（trampoline 约定；实际指向 ctxs 数组中该子项的上下文）。
/// @param c 布局器为该子项分配的约束。
/// @return 子控件 `layout()` 在给定约束下返回的尺寸。
inline auto container_measure(void *ctx, const Constraints &c) -> Size {
    const auto *lc = static_cast<ContainerLayoutCtx *>(ctx);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) trampoline 用裸指针+索引解包子节点
    return lc->children[lc->index].widget().layout(c, *lc->build_ctx);
}

/// @brief ContainerLayoutCtx 的基线 trampoline（`FlexItem::BaselineFn`）：
///        解包子控件 → 取控件级基线 → 补上内容盒位移，得到「布局盒顶 → 基线」。
///
/// 语义链：`Widget::baseline_distance(ctx)` 给出「**内容盒**顶 → 首行基线」（含控件自身内边距），
/// 而布局器需要的是「**布局盒**顶 → 基线」，两者相差 `Modifier::transform(measured).translation`
/// 的 y 分量（`Modifier::padding` / Align 造成的内容盒平移）。子控件无基线时返回 `nullopt`，
/// 布局器对之走 CSS 式合成基线（交叉轴底边），不报错。
///
/// `measured` 由布局器传入（测量期的子项尺寸），因此本函数**不**依赖 `Node::bounds`——
/// 布局器是在 `Row/Column::on_layout` 的 `set_bounds` 之前被调用的。
/// @param ctx 指向 `ContainerLayoutCtx` 的不透明指针（trampoline 约定）。
/// @param measured 布局器传入的子项测量尺寸（用于查询 modifier 的内容盒平移量）。
/// @return 「布局盒顶 → 首行基线」距离；子控件无基线时为 `nullopt`（布局器走合成基线）。
inline auto container_baseline(void *ctx, Size measured) -> std::optional<float> {
    const auto *lc = static_cast<ContainerLayoutCtx *>(ctx);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) trampoline 用裸指针+索引解包子节点
    const Widget &child = lc->children[lc->index].widget();
    const std::optional<float> baseline = child.baseline_distance(*lc->build_ctx);
    if (!baseline.has_value()) {
        return std::nullopt;
    }
    return *baseline + child.modifier().transform(measured).translation.y;
}

/// @brief `Column` 的属性权威表：一行一字段，键名、私有字段、脏档位与回落点只在行里声明一次。
///
/// 四行全是**常量档**（回落点是库内默认提供者，不随主题变），故读件一律无参；四档都只打布局脏。
///
/// 子节点不在本表里：子项只能随控件构造移入（`Column(std::vector<Node>, ColumnProps)`），
/// 属性对象不承担容器内容 —— 「子项在构造后仍要移动、而属性对象不是子项所有者」那类错位
/// 因此不会落到表上。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class ColumnProps : public AbstractProps<ColumnProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：主轴对齐策略（纵向主轴上有基线语义）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto main_axis_alignment_row() {
        return prop_row<&ColumnProps::main_axis_alignment_>("main_axis_alignment")
            .layout()
            .fallback(&ColumnProps::main_axis_alignment_default)
            .note("Main axis alignment");
    }

    /// @brief 可空值件：交叉轴对齐策略（`Column` 的交叉轴是水平的）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto cross_axis_alignment_row() {
        return prop_row<&ColumnProps::cross_axis_alignment_>("cross_axis_alignment")
            .layout()
            .fallback(&ColumnProps::cross_axis_alignment_default)
            .note("Cross axis alignment");
    }

    /// @brief 可空值件：主轴尺寸策略（贴合内容 / 撑满约束）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto main_axis_size_row() {
        return prop_row<&ColumnProps::main_axis_size_>("main_axis_size")
            .layout()
            .fallback(&ColumnProps::main_axis_size_default)
            .note("Main axis size policy");
    }

    /// @brief 可空值件：相邻子项固定间距（dp）；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto gap_row() {
        return prop_row<&ColumnProps::gap_>("gap")
            .layout()
            .bounds("0")
            .fallback(&ColumnProps::gap_default)
            .note("Child spacing (px)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(main_axis_alignment_row(), cross_axis_alignment_row(), main_axis_size_row(), gap_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前生效的主轴对齐策略。
    /// @return 生效的对齐策略；未表态时为 `Start`。
    [[nodiscard]] auto main_axis_alignment() const -> MainAxisAlignment { return stated(main_axis_alignment_row()); }

    /// @brief 当前生效的交叉轴对齐策略。
    /// @return 生效的对齐策略；未表态时为 `Start`。
    [[nodiscard]] auto cross_axis_alignment() const -> CrossAxisAlignment { return stated(cross_axis_alignment_row()); }

    /// @brief 当前生效的主轴尺寸策略。
    /// @return 生效的策略；未表态时为 `Min`（贴合内容）。
    [[nodiscard]] auto main_axis_size() const -> MainAxisSize { return stated(main_axis_size_row()); }

    /// @brief 当前生效的相邻子项间距（dp）。
    /// @return 生效间距；未表态时为 0。
    [[nodiscard]] auto gap() const -> float { return stated(gap_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入主轴对齐策略。
    /// @param value 新策略；表态即脱离 `Start` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_main_axis_alignment(MainAxisAlignment value) -> ColumnProps & {
        return assign(main_axis_alignment_row(), value);
    }

    /// @brief 装入交叉轴对齐策略。
    /// @param value 新策略；表态即脱离 `Start` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_cross_axis_alignment(CrossAxisAlignment value) -> ColumnProps & {
        return assign(cross_axis_alignment_row(), value);
    }

    /// @brief 装入主轴尺寸策略。
    /// @param value 新策略；表态即脱离 `Min` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_main_axis_size(MainAxisSize value) -> ColumnProps & { return assign(main_axis_size_row(), value); }

    /// @brief 装入相邻子项间距（dp）。
    /// @param value 新间距；0 是「显式不留间距」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_gap(float value) -> ColumnProps & { return assign(gap_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 主轴对齐回落基线。
    /// @return `MainAxisAlignment::Start`。
    [[nodiscard]] static auto main_axis_alignment_default() -> MainAxisAlignment { return MainAxisAlignment::Start; }

    /// @brief 交叉轴对齐回落基线。
    /// @return `CrossAxisAlignment::Start`。
    [[nodiscard]] static auto cross_axis_alignment_default() -> CrossAxisAlignment { return CrossAxisAlignment::Start; }

    /// @brief 主轴尺寸策略回落基线。
    /// @return `MainAxisSize::Min`（贴合内容）。
    [[nodiscard]] static auto main_axis_size_default() -> MainAxisSize { return MainAxisSize::Min; }

    /// @brief 间距回落基线。
    /// @return 0.0F（不插入额外间距）。
    [[nodiscard]] static auto gap_default() -> float { return 0.0F; }

  private:
    std::optional<MainAxisAlignment> main_axis_alignment_;  ///< 主轴对齐表态位
    std::optional<CrossAxisAlignment> cross_axis_alignment_;  ///< 交叉轴对齐表态位
    std::optional<MainAxisSize> main_axis_size_;  ///< 主轴尺寸策略表态位
    std::optional<float> gap_;  ///< 子项间距表态位（dp）

    friend class AbstractProps<ColumnProps>;
};

/// @brief 纵向线性布局容器（主轴 = 垂直）：属性面全部经 `ColumnProps` 的读写件访问。
///
/// 通过 `FlexLayouter` 完成两阶段布局：子项按 flex 权重瓜分剩余高度，交叉轴取最宽子项；
/// 主轴/交叉轴对齐与 `Expand`（经 `Modifier::expand`）见 specification/03-layout-render.md §7.2。
///
/// 子节点不入属性表：它们只能随构造移入（`Column(std::vector<Node>, ColumnProps)` 或
/// `Column{ a, b }`），运行期增删走 `Container::add` / `remove_child`。反向排布是控件级开关
/// （`set_reverse`），既不是属性行也不序列化 —— 与迁移前 `flex.direction` 的可达面一致。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Column : public Container, public ColumnProps {
  public:
    /// @brief 默认构造：无子项、各行未表态（生效值即回落点），并接线宿主。
    Column() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 列属性对象，移动进本实例的 `ColumnProps` 基类子对象。
    explicit Column(ColumnProps props) : ColumnProps(std::move(props)) { bind(*this); }

    /// @brief 子项 + 配置块构造：子项整体移入容器，属性接管后接线宿主。
    /// @param kids 初始子节点（整体移入 `children_`，节点身份不变，外部 `shared_ptr` 别名继续有效）。
    /// @param props 列属性对象；默认全未表态，生效值即各行回落点。
    Column(std::vector<Node> kids, ColumnProps props = {}) : ColumnProps(std::move(props)) {
        adopt_children(std::move(kids));
        bind(*this);
    }

    /// @brief 便捷构造：扁平罗列子项（Column{ a, b }），免写 Node{} 与 Props 包裹。
    /// @param kids 初始子项列表（经 set_children 存入容器）。
    Column(std::initializer_list<Node> kids) {
        set_children(kids);
        bind(*this);
    }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `ColumnProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> ColumnProps { return {}; }

    /// @brief 设置相邻子项间距（链式）；转调 `ColumnProps::set_gap`，返回控件引用。
    /// @param g 间距（dp；负值由 `validate_props` 拦截）。
    /// @return Column 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_gap(float g) -> Column & {
        ColumnProps::set_gap(g);
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    /// @param a 主轴对齐策略。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_main_axis_alignment(MainAxisAlignment a) -> Column & {
        ColumnProps::set_main_axis_alignment(a);
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    /// @param a 交叉轴对齐策略。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Column & {
        ColumnProps::set_cross_axis_alignment(a);
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    /// @param s 主轴尺寸策略（Min 贴合内容 / Max 撑满约束）。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_main_axis_size(MainAxisSize s) -> Column & {
        ColumnProps::set_main_axis_size(s);
        return *this;
    }

    /// @brief 设置反向排布（沿主轴镜像子项顺序与 `Start/End` 语义）。
    /// @param value true 即反向；本开关不进属性表、不序列化。
    /// @return *this 引用（链式调用）。
    auto set_reverse(bool value = true) -> Column & {
        if (reverse_ != value) {
            reverse_ = value;
            mark_needs_layout();
        }
        return *this;
    }

    /// @brief 当前是否反向排布。
    /// @return 反向时为 true。
    [[nodiscard]] auto reverse() const -> bool { return reverse_; }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "Column"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "Column"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Column" 的描述表：`properties` 按 `rows()` 点名顺序，无事件、可容纳多个子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Column");
        descriptor.children_policy = "multiple";
        descriptor.invariants = {"gap >= 0"};
        descriptor.examples = {R"(au::Column{ au::Text("A"), au::Text("B") })",
                               R"(au::Column({ au::Text("A"), au::Text("B") }, au::ColumnProps().set_gap(8.0F)))"};
        return descriptor;
    }

    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（名称/属性/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化自有属性到 `props`：各行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param props 写入目标 JSON 对象（就地填充键值）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从属性 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 读取来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    /// @return gap 为负时返回 WidgetInvalidProp 错误；否则返回成功。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap() < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap()),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // Column 的交叉轴是水平的：`CrossAxisAlignment::Baseline` 没有基线语义，布局器按 `Start` 处理。
        // 一次性降级提示（每实例仅一次，不每帧刷屏）——放在布局入口可同时覆盖 setter / from_json 两条路径，
        // 无需在每处状态变更点重复接线。
        if (cross_axis_alignment() == CrossAxisAlignment::Baseline && !baseline_degraded_warned_) {
            baseline_degraded_warned_ = true;
            Diagnostics::degraded(
                "The cross axis of Column is horizontal, CrossAxisAlignment::Baseline has no "
                "baseline semantics, handled as Start",
                "layout");
        }
        std::vector<ContainerLayoutCtx> ctxs(children_.size());  // 每个子项一个布局上下文槽（与 children_ 等长）
        std::vector<FlexItem> items;  // 传给 FlexLayouter 的子项度量视图
        items.reserve(children_.size());  // 预置与子项数等量容量，避免逐项扩容
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg;  // 本帧生效 flex 配置：轴向由控件类型固定，其余四档取各行生效值
        cfg.direction = reverse_ ? FlexDirection::ColumnReverse : FlexDirection::Column;
        cfg.main_axis = main_axis_alignment();
        cfg.cross_axis = cross_axis_alignment();
        cfg.main_axis_size = main_axis_size();
        cfg.gap = gap();
        // 布局镜像：按生效书写方向（Environment/进程级）翻转水平排布。
        cfg.rtl = resolved_text_direction(ctx) == TextDirection::RTL;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < children_.size(); ++i) {
            children_[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }

  private:
    bool reverse_ = false;  ///< 反向排布（控件级开关，不进属性表、不序列化）
    bool baseline_degraded_warned_ = false;  ///< `Baseline` 降级提示是否已发（每实例一次，避免逐帧刷屏）
};

/// @brief `Row` 的属性权威表：与 `ColumnProps` 同行同序，交叉轴语义随主轴方向对调。
///
/// 四行全是**常量档**（回落点是库内默认提供者，不随主题变），故读件一律无参；子节点同样不入表。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class RowProps : public AbstractProps<RowProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：主轴对齐策略。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto main_axis_alignment_row() {
        return prop_row<&RowProps::main_axis_alignment_>("main_axis_alignment")
            .layout()
            .fallback(&RowProps::main_axis_alignment_default)
            .note("Main axis alignment");
    }

    /// @brief 可空值件：交叉轴对齐策略（`Row` 的交叉轴是垂直的）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto cross_axis_alignment_row() {
        return prop_row<&RowProps::cross_axis_alignment_>("cross_axis_alignment")
            .layout()
            .fallback(&RowProps::cross_axis_alignment_default)
            .note("Cross axis alignment");
    }

    /// @brief 可空值件：主轴尺寸策略（贴合内容 / 撑满约束）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto main_axis_size_row() {
        return prop_row<&RowProps::main_axis_size_>("main_axis_size")
            .layout()
            .fallback(&RowProps::main_axis_size_default)
            .note("Main axis size policy");
    }

    /// @brief 可空值件：相邻子项固定间距（dp）；下界 0。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto gap_row() {
        return prop_row<&RowProps::gap_>("gap")
            .layout()
            .bounds("0")
            .fallback(&RowProps::gap_default)
            .note("Child spacing (px)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(main_axis_alignment_row(), cross_axis_alignment_row(), main_axis_size_row(), gap_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前生效的主轴对齐策略。
    /// @return 生效的对齐策略；未表态时为 `Start`。
    [[nodiscard]] auto main_axis_alignment() const -> MainAxisAlignment { return stated(main_axis_alignment_row()); }

    /// @brief 当前生效的交叉轴对齐策略。
    /// @return 生效的对齐策略；未表态时为 `Start`。
    [[nodiscard]] auto cross_axis_alignment() const -> CrossAxisAlignment { return stated(cross_axis_alignment_row()); }

    /// @brief 当前生效的主轴尺寸策略。
    /// @return 生效的策略；未表态时为 `Min`（贴合内容）。
    [[nodiscard]] auto main_axis_size() const -> MainAxisSize { return stated(main_axis_size_row()); }

    /// @brief 当前生效的相邻子项间距（dp）。
    /// @return 生效间距；未表态时为 0。
    [[nodiscard]] auto gap() const -> float { return stated(gap_row()); }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入主轴对齐策略。
    /// @param value 新策略；表态即脱离 `Start` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_main_axis_alignment(MainAxisAlignment value) -> RowProps & {
        return assign(main_axis_alignment_row(), value);
    }

    /// @brief 装入交叉轴对齐策略。
    /// @param value 新策略；表态即脱离 `Start` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_cross_axis_alignment(CrossAxisAlignment value) -> RowProps & {
        return assign(cross_axis_alignment_row(), value);
    }

    /// @brief 装入主轴尺寸策略。
    /// @param value 新策略；表态即脱离 `Min` 基线。
    /// @return 引用本属性对象，便于链式续写。
    auto set_main_axis_size(MainAxisSize value) -> RowProps & { return assign(main_axis_size_row(), value); }

    /// @brief 装入相邻子项间距（dp）。
    /// @param value 新间距；0 是「显式不留间距」，与「未表态」不是同一件事。
    /// @return 引用本属性对象，便于链式续写。
    auto set_gap(float value) -> RowProps & { return assign(gap_row(), value); }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 主轴对齐回落基线。
    /// @return `MainAxisAlignment::Start`。
    [[nodiscard]] static auto main_axis_alignment_default() -> MainAxisAlignment { return MainAxisAlignment::Start; }

    /// @brief 交叉轴对齐回落基线。
    /// @return `CrossAxisAlignment::Start`。
    [[nodiscard]] static auto cross_axis_alignment_default() -> CrossAxisAlignment { return CrossAxisAlignment::Start; }

    /// @brief 主轴尺寸策略回落基线。
    /// @return `MainAxisSize::Min`（贴合内容）。
    [[nodiscard]] static auto main_axis_size_default() -> MainAxisSize { return MainAxisSize::Min; }

    /// @brief 间距回落基线。
    /// @return 0.0F（不插入额外间距）。
    [[nodiscard]] static auto gap_default() -> float { return 0.0F; }

  private:
    std::optional<MainAxisAlignment> main_axis_alignment_;  ///< 主轴对齐表态位
    std::optional<CrossAxisAlignment> cross_axis_alignment_;  ///< 交叉轴对齐表态位
    std::optional<MainAxisSize> main_axis_size_;  ///< 主轴尺寸策略表态位
    std::optional<float> gap_;  ///< 子项间距表态位（dp）

    friend class AbstractProps<RowProps>;
};

/// @brief 横向线性布局容器（主轴 = 水平）：属性面全部经 `RowProps` 的读写件访问。
///
/// 子节点不入属性表（随构造移入，运行期增删走 `Container::add` / `remove_child`）；反向排布是
/// 控件级开关（`set_reverse`），不序列化。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Row : public Container, public RowProps {
  public:
    /// @brief 默认构造：无子项、各行未表态（生效值即回落点），并接线宿主。
    Row() { bind(*this); }

    /// @brief 配置块构造（specification/04-widget.md §2.5）：整份属性接管后接线宿主。
    /// @param props 行属性对象，移动进本实例的 `RowProps` 基类子对象。
    explicit Row(RowProps props) : RowProps(std::move(props)) { bind(*this); }

    /// @brief 子项 + 配置块构造：子项整体移入容器，属性接管后接线宿主。
    /// @param kids 初始子节点（整体移入 `children_`，节点身份不变，外部 `shared_ptr` 别名继续有效）。
    /// @param props 行属性对象；默认全未表态，生效值即各行回落点。
    Row(std::vector<Node> kids, RowProps props = {}) : RowProps(std::move(props)) {
        adopt_children(std::move(kids));
        bind(*this);
    }

    /// @brief 便捷构造：扁平罗列子项（Row{ a, b }），免写 Node{} 与 Props 包裹。
    /// @param kids 初始子项列表（经 set_children 存入容器）。
    Row(std::initializer_list<Node> kids) {
        set_children(kids);
        bind(*this);
    }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源，需求 SPEC.API.SENSIBLE-DEFAULTS.001）。
    /// @return 一份缺省构造的 `RowProps`：各行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> RowProps { return {}; }

    /// @brief 设置相邻子项间距（链式）；转调 `RowProps::set_gap`，返回控件引用。
    /// @param g 间距（dp；负值由 `validate_props` 拦截）。
    /// @return Row 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_gap(float g) -> Row & {
        RowProps::set_gap(g);
        return *this;
    }

    /// @brief 设置主轴对齐方式（MainAxisAlignment）。
    /// 仅当容器主轴尺寸大于子项占用（例如 `set_main_axis_size(MainAxisSize::Max)`
    /// 或父约束强制更大）时才有可见自由空间。
    /// @param a 主轴对齐策略。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_main_axis_alignment(MainAxisAlignment a) -> Row & {
        RowProps::set_main_axis_alignment(a);
        return *this;
    }

    /// @brief 设置交叉轴对齐方式（CrossAxisAlignment）。`Stretch` 会拉伸子项填满交叉轴。
    /// @param a 交叉轴对齐策略。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_cross_axis_alignment(CrossAxisAlignment a) -> Row & {
        RowProps::set_cross_axis_alignment(a);
        return *this;
    }

    /// @brief 设置主轴尺寸策略。`Max` 使容器撑满父级可用主轴空间，从而让 `main_axis_alignment` 产生可见效果。
    /// @param s 主轴尺寸策略（Min 贴合内容 / Max 撑满约束）。
    /// @return *this 引用（链式调用）。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_main_axis_size(MainAxisSize s) -> Row & {
        RowProps::set_main_axis_size(s);
        return *this;
    }

    /// @brief 设置反向排布（沿主轴镜像子项顺序与 `Start/End` 语义）。
    /// @param value true 即反向；本开关不进属性表、不序列化。
    /// @return *this 引用（链式调用）。
    auto set_reverse(bool value = true) -> Row & {
        if (reverse_ != value) {
            reverse_ = value;
            mark_needs_layout();
        }
        return *this;
    }

    /// @brief 当前是否反向排布。
    /// @return 反向时为 true。
    [[nodiscard]] auto reverse() const -> bool { return reverse_; }

    /// @brief 控件类型名（Inspector / 序列化路由用）。
    /// @return 静态字符串字面量 "Row"，生命周期同程序。
    [[nodiscard]] auto type_name() const -> const char * override { return "Row"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "Row" 的描述表：`properties` 按 `rows()` 点名顺序，无事件、可容纳多个子节点。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("Row");
        descriptor.children_policy = "multiple";
        descriptor.invariants = {"gap >= 0"};
        descriptor.examples = {R"(au::Row{ au::Text("A"), au::Text("B") })",
                               R"(au::Row({ au::Text("A"), au::Text("B") }, au::RowProps().set_gap(8.0F)))"};
        return descriptor;
    }

    /// @brief 实例级自描述：转发静态描述表。
    /// @return 与 describe_static() 相同的 WidgetDescriptor（名称/属性/不变量/示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 序列化自有属性到 `props`：各行只在表态时写（未表态不写键，回落语义不外泄）。
    /// @param props 写入目标 JSON 对象（就地填充键值）。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props_serialize(props);
    }

    /// @brief 从属性 JSON 重建自有属性（与 `serialize_props` 对偶；键缺失则保留现值）。
    /// @note 非法值一律不写入：可空行撤表态（生效值退回该行回落点）并经 Diagnostics 上报。
    /// @param props 读取来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        props_deserialize(props);
    }

    /// @brief 构建期属性约束校验（specification/04-widget.md §2.2）：校验 gap >= 0。
    /// @return gap 为负时返回 WidgetInvalidProp 错误；否则返回成功。
    [[nodiscard]] auto validate_props() const -> Result<void> override {
        if (gap() < 0.0F) {
            return make_error(ErrorCode::WidgetInvalidProp, "Layout gap must be >= 0, got " + std::to_string(gap()),
                              "Use non-negative spacing");
        }
        return Result<void>{};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        std::vector<ContainerLayoutCtx> ctxs(children_.size());  // 每个子项一个布局上下文槽（与 children_ 等长）
        std::vector<FlexItem> items;  // 传给 FlexLayouter 的子项度量视图
        items.reserve(children_.size());  // 预置与子项数等量容量，避免逐项扩容
        for (size_t i = 0; i < children_.size(); ++i) {
            ctxs[i] = ContainerLayoutCtx{{}, children_.data(), i, &ctx};
            const float w = children_[i].widget().modifier().flex_weight();
            // 基线通道一并挂上：布局器仅在 `cross_axis == Baseline` 时调用（其余配置零调用开销）。
            items.push_back(
                FlexItem::make<ContainerLayoutCtx>(w, &ctxs[i], container_measure, container_baseline, &ctxs[i]));
        }
        Flex cfg;  // 本帧生效 flex 配置：轴向由控件类型固定，其余四档取各行生效值
        cfg.direction = reverse_ ? FlexDirection::RowReverse : FlexDirection::Row;
        cfg.main_axis = main_axis_alignment();
        cfg.cross_axis = cross_axis_alignment();
        cfg.main_axis_size = main_axis_size();
        cfg.gap = gap();
        // 布局镜像：按生效书写方向（Environment/进程级）翻转水平排布。
        cfg.rtl = resolved_text_direction(ctx) == TextDirection::RTL;
        const FlexLayout result = FlexLayouter::layout(cfg, c, items);
        for (size_t i = 0; i < children_.size(); ++i) {
            children_[i].set_bounds(result.children[i]);
        }
        return c.constrain(result.size);
    }

  private:
    bool reverse_ = false;  ///< 反向排布（控件级开关，不进属性表、不序列化）
};

}  // namespace aurora
