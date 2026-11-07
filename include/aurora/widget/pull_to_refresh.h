#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"
#include "aurora/core/color.h"
#include "aurora/environment/build_context.h"  // 带上下文的读件需完整类型以解主题作用域
#include "aurora/event/gesture.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/color_roles.h"  // 活令牌回落点：roles.primary / surface / on_surface_variant
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 下拉刷新状态机（PullToRefresh 三态）。
enum class PullToRefreshState : std::uint8_t {
    Idle,  ///< 空闲（指示器收起）
    Pulling,  ///< 下拉中（橡皮筋跟手）
    Refreshing,  ///< 刷新中（spinner 持续旋转，待 `finish_refresh()` 收拢）
};

/// @brief `PullToRefresh` 的属性权威表：阈值与上限两行常量档、三处主题色三行活令牌档，
///        被包裹的滚动子树不入表。
///
/// 五行全部是**绘制档**（`.paint()`）：阈值只被 `release_pull` 的触发裁决与 `progress()` 读取，
/// 上限只被橡皮筋映射与回弹夹取读取，三处主题色只在 `on_paint` 的覆盖层里取用——没有一行参与布局，
/// 写入一律只标脏 paint。
///
/// 子树不在本表里：它只能随构造移入（`PullToRefresh(Node)` 或
/// `PullToRefresh(Node, PullToRefreshProps)`），构造后再给「子节点」赋值无处安放——`SingleChild`
/// 的唯一插槽是 protected，运行期换子走容器自身的 `set_child`。
///
/// @note Thread: main-thread only
/// @note Side-effects: 写入经宿主链接位标脏；未挂载时是纯 DTO
class PullToRefreshProps : public AbstractProps<PullToRefreshProps> {
  public:
    // ---- 权威表：参数顺序即 schema 顺序 ----

    /// @brief 可空值件：触发刷新的下拉距离（dp）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto threshold_row() {
        return prop_row<&PullToRefreshProps::threshold_>("threshold")
            .paint()
            .fallback(&PullToRefreshProps::threshold_default)
            .note("Pull distance that triggers a refresh (dp)");
    }

    /// @brief 可空值件：橡皮筋下拉上限（dp）。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto max_pull_row() {
        return prop_row<&PullToRefreshProps::max_pull_>("max_pull")
            .paint()
            .fallback(&PullToRefreshProps::max_pull_default)
            .note("Rubber-band pull limit (dp)");
    }

    /// @brief 可空值件：下拉指示器强调色；未表态时跟随主题 `roles.primary`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto indicator_color_row() {
        return prop_row<&PullToRefreshProps::indicator_color_>("indicator_color")
            .paint()
            .fallback<&ColorRoles::primary>()
            .note("Pull indicator accent (follows theme primary when unset)");
    }

    /// @brief 可空值件：顶部覆盖层背景色；未表态时跟随主题 `roles.surface`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto band_color_row() {
        return prop_row<&PullToRefreshProps::band_color_>("band_color")
            .paint()
            .fallback<&ColorRoles::surface>()
            .note("Top overlay band background (follows theme surface when unset)");
    }

    /// @brief 可空值件：「刷新中…」提示文字色；未表态时跟随主题 `roles.on_surface_variant`。
    /// @return 本行的表行对象。
    [[nodiscard]] static constexpr auto refreshing_text_color_row() {
        return prop_row<&PullToRefreshProps::refreshing_text_color_>("refreshing_text_color")
            .paint()
            .fallback<&ColorRoles::on_surface_variant>()
            .note("Refreshing hint text color (follows theme on_surface_variant when unset)");
    }

    /// @brief 权威表本体：本控件全部属性行的元组。
    /// @return 按声明顺序排列的行元组（发射面与序列化面都遍历它）。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(threshold_row(), max_pull_row(), indicator_color_row(), band_color_row(),
                              refreshing_text_color_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定 ----

    /// @brief 当前生效的触发距离（dp）。
    /// @return 生效距离；未表态时为 64。
    [[nodiscard]] auto threshold() const -> float { return stated(threshold_row()); }

    /// @brief 当前生效的橡皮筋下拉上限（dp）。
    /// @return 生效上限；未表态时为 128。
    [[nodiscard]] auto max_pull() const -> float { return stated(max_pull_row()); }

    /// @brief 当前生效的下拉指示器强调色。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 生效强调色；未表态时为当前主题的 `roles.primary`。
    [[nodiscard]] auto indicator_color(const BuildContext &ctx) const -> Color {
        return resolve(indicator_color_row(), ctx);
    }

    /// @brief 当前生效的顶部覆盖层背景色。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 生效背景色；未表态时为当前主题的 `roles.surface`。
    [[nodiscard]] auto band_color(const BuildContext &ctx) const -> Color { return resolve(band_color_row(), ctx); }

    /// @brief 当前生效的「刷新中…」提示文字色。
    /// @param ctx 构建上下文：提供主题作用域。
    /// @return 生效文字色；未表态时为当前主题的 `roles.on_surface_variant`。
    [[nodiscard]] auto refreshing_text_color(const BuildContext &ctx) const -> Color {
        return resolve(refreshing_text_color_row(), ctx);
    }

    // ---- 写面：实参一律裸 T；一次写入即「表态」，未表态才轮到回落点 ----

    /// @brief 装入触发距离（dp）。
    /// @param value 新距离；原样存入（非正值由使用点的夹取兜底，映射后不会产生下拉）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_threshold(float value) -> PullToRefreshProps & { return assign(threshold_row(), value); }

    /// @brief 装入橡皮筋下拉上限（dp）。
    /// @param value 新上限；原样存入（0 表示不产生下拉，与「未表态」不是同一件事）。
    /// @return 引用本属性对象，便于链式续写。
    auto set_max_pull(float value) -> PullToRefreshProps & { return assign(max_pull_row(), value); }

    /// @brief 装入下拉指示器强调色。
    /// @param value 新强调色；表态后不再跟随主题换肤。
    /// @return 引用本属性对象，便于链式续写。
    auto set_indicator_color(Color value) -> PullToRefreshProps & { return assign(indicator_color_row(), value); }

    /// @brief 装入顶部覆盖层背景色。
    /// @param value 新背景色；表态后不再跟随主题换肤。
    /// @return 引用本属性对象，便于链式续写。
    auto set_band_color(Color value) -> PullToRefreshProps & { return assign(band_color_row(), value); }

    /// @brief 装入「刷新中…」提示文字色。
    /// @param value 新文字色；表态后不再跟随主题换肤。
    /// @return 引用本属性对象，便于链式续写。
    auto set_refreshing_text_color(Color value) -> PullToRefreshProps & {
        return assign(refreshing_text_color_row(), value);
    }

    // ---- 回落点提供者：常量档的唯一基线（与迁移前的字段声明处默认值逐值相同） ----

    /// @brief 触发距离回落基线。
    /// @return 64.0F。
    [[nodiscard]] static auto threshold_default() -> float { return 64.0F; }

    /// @brief 橡皮筋下拉上限回落基线。
    /// @return 128.0F。
    [[nodiscard]] static auto max_pull_default() -> float { return 128.0F; }

  private:
    std::optional<float> threshold_;  ///< 触发距离表态位（dp）
    std::optional<float> max_pull_;  ///< 橡皮筋下拉上限表态位（dp）
    std::optional<Color> indicator_color_;  ///< 指示器强调色表态位
    std::optional<Color> band_color_;  ///< 覆盖层背景色表态位
    std::optional<Color> refreshing_text_color_;  ///< 提示文字色表态位

    friend class AbstractProps<PullToRefreshProps>;
};

/// @brief 下拉刷新容器（specification/04-widget.md）：包住一个滚动子树，顶部继续下拉时
/// 积累下拉距离，越过阈值松手触发 `on_refresh` 回调，`finish_refresh()` 收拢回弹。
///
/// 两条输入通道（均要求滚动子树已在顶部）：
/// - **拖拽**：`DragRecognizer` 垂直锁定主轴后 0.45 系数橡皮筋映射（远端渐硬）；
/// - **滚轮余量**：嵌套滚动协调下，子级把到顶后未消费的量经 `ScrollEvent::remaining_y`
/// 上冒给本容器（本容器在命中链上位于子滚动控件更浅层）。
///
/// 指示器以**覆盖层**绘制在视口顶部（不动布局盒、不推挤内容，spec §6.5「动画不改布局」），
/// 回弹/收拢走共享 `ScrollGlide` 短滑动时序（自驱动 tick，不占 `Animator`，步长在消费点经
/// `scaled_frame_dt` 折算一次）；
/// reduce-motion 下直落端点、不产生中间帧（对齐 `AnimationController::tick` 短路语义）。
///
/// 属性面全部经 `PullToRefreshProps` 的读写件访问；子树不入属性表：它只能随构造移入
/// （`PullToRefresh(Node)` 或 `PullToRefresh(Node, PullToRefreshProps)`）。
///
/// 对标 Flutter `RefreshIndicator`、Material 3 pull-to-refresh、iOS UIRefreshControl。
///
/// @note Thread: main-thread only
/// @note Rebuildable: yes（阈值/上限/三处主题色可序列化；`on_refresh` 回调属运行时接线，重建后须重挂）
class PullToRefresh : public SingleChild, public PullToRefreshProps {
  public:
    /// @brief 默认构造：空子树、五行未表态（生效值即回落点），并接线宿主。
    /// 重建路径（serialization 工厂）使用此构造——子树与回调由宿主回填。
    PullToRefresh() { bind(*this); }

    /// @brief 以下拉目标子树构造（属性全未表态）。
    /// @param child 被包裹的滚动子树；空节点时保持无子。
    /// @param props 下拉刷新属性对象；默认全未表态，生效值即各行回落点。
    explicit PullToRefresh(Node child, PullToRefreshProps props = {})
        : SingleChild(std::move(child)), PullToRefreshProps(std::move(props)) {
        bind(*this);
    }

    /// @brief 以属性聚合构造（子树另经容器构造交入）。
    /// @param props 下拉刷新属性对象，移动进本实例的 `PullToRefreshProps` 基类子对象。
    explicit PullToRefresh(PullToRefreshProps props) : PullToRefreshProps(std::move(props)) { bind(*this); }

    /// @brief 运行时可查询的默认属性值（属性表的单一事实来源）。
    /// @return 一份缺省构造的 `PullToRefreshProps`：五行未表态，生效值即该行回落点。
    [[nodiscard]] static auto defaults() -> PullToRefreshProps { return {}; }

    /// @brief 类型名字符串 "PullToRefresh"。
    /// @return 静态字符串常量，指向类型名。
    [[nodiscard]] auto type_name() const -> const char * override { return "PullToRefresh"; }

    /// @brief 运行时自描述（规格附录 B）：属性项全部由权威表发射，控件只补自己的事实。
    /// @return 名为 "PullToRefresh" 的描述表：`properties` 按 `rows()` 点名顺序，事件 `on_refresh`，
    ///         子策略 single。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        auto descriptor = props_descriptor("PullToRefresh");
        descriptor.events = {"on_refresh"};
        descriptor.children_policy = "single";
        descriptor.examples = {"au::PullToRefresh(au::Scroll{...}) /* pull down at the top to trigger a refresh */"};
        return descriptor;
    }
    /// @brief 运行时自描述（规格附录 B）。
    /// @return 与 `describe_static()` 相同的描述符。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集响应式来源（表内来源行由表派生；本控件五行均非来源行）。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

    /// @brief 注册刷新回调（松手越过阈值时触发一次；回调内异步取数，完成后调 `finish_refresh`）。
    /// @param cb 无参刷新回调；覆盖既有值。
    /// @return 自身引用，便于链式调用。
    auto on_refresh(std::function<void()> cb) -> PullToRefresh & {
        on_refresh_ = std::move(cb);
        return *this;
    }

    /// @brief 设置触发距离（链式）；转调 `PullToRefreshProps::set_threshold`，返回控件引用。
    /// @param value 新距离（dp）；写入件原样存入。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_threshold(float value) -> PullToRefresh & {
        PullToRefreshProps::set_threshold(value);
        return *this;
    }

    /// @brief 设置橡皮筋下拉上限（链式）；转调 `PullToRefreshProps::set_max_pull`，返回控件引用。
    /// @param value 新上限（dp）；0 表示不产生下拉。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_max_pull(float value) -> PullToRefresh & {
        PullToRefreshProps::set_max_pull(value);
        return *this;
    }

    /// @brief 设置下拉指示器强调色（链式）；表态后不再跟随主题 `roles.primary`。
    /// @param value 强调色（spinner 弧线）。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_indicator_color(Color value) -> PullToRefresh & {
        PullToRefreshProps::set_indicator_color(value);
        return *this;
    }

    /// @brief 设置顶部覆盖层背景色（链式）；表态后不再跟随主题 `roles.surface`。
    /// @param value 覆盖层带背景色。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_band_color(Color value) -> PullToRefresh & {
        PullToRefreshProps::set_band_color(value);
        return *this;
    }

    /// @brief 设置「刷新中…」提示文字色（链式）；表态后不再跟随主题 `roles.on_surface_variant`。
    /// @param value 提示文字色。
    /// @return 自身引用，便于链式调用。
    /// NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method) 同名控件面写件（返回控件引用）
    auto set_refreshing_text_color(Color value) -> PullToRefresh & {
        PullToRefreshProps::set_refreshing_text_color(value);
        return *this;
    }

    /// @brief 结束刷新态：指示器收拢回弹（reduce-motion 下直落 0）。数据加载完成后调用。
    auto finish_refresh() -> void {
        if (state_ != PullToRefreshState::Refreshing) {
            return;
        }
        state_ = PullToRefreshState::Idle;
        begin_setback_glide(0.0F);
    }

    /// @brief 当前状态机三态。
    /// @return Idle / Pulling / Refreshing 之一。
    [[nodiscard]] auto state() const -> PullToRefreshState { return state_; }
    /// @brief 当前下拉距离（dp，覆盖层高度；测试/联动观测点）。
    /// @return 橡皮筋映射后的下拉量 pull_。
    [[nodiscard]] auto pull_distance() const -> float { return pull_; }
    /// @brief 触发进度（pull/threshold，可 >1；绑定指示器旋转角度/透明度用）。
    /// @return 下拉距离与阈值之比；阈值非正时为 0。
    [[nodiscard]] auto progress() const -> float { return threshold() > 0.0F ? pull_ / threshold() : 0.0F; }
    /// @brief 是否正在回弹滑动（测试观测点）。
    /// @return 短滑动时序仍处于活跃状态时为 true。
    [[nodiscard]] auto is_gliding() const -> bool { return glide_.active; }

    /// @brief 序列化：通用属性 + 自有各行（回调不序列化）。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);  // 先由基类写入通用属性（width/height/show 等）
        props_serialize(props);  // 各行只在表态时写：未表态不写键，保持「跟随回落点」语义
    }
    /// @brief 反序列化：恢复通用属性与自有各行（缺失键保持当前值）。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);  // 先由基类恢复通用属性
        props_deserialize(props);
    }

    /// @brief 滚轮余量消费（嵌套滚动协调）：派发器把子级到顶后冒上来的余量改写为
    ///        `delta_y` 再交给本容器（`remaining_y` 每跳重置，不作为入口信号）。
    ///        仅吃「向上滚且子树已在顶部」的量——门控不通过时按原样回传 `remaining_y`
    ///        继续上冒（派发器把「未写余量」视为全量消费，静默 return 会吞掉事件）。
    /// @param e 滚动事件；消费时置 `is_handled` 并清零 `remaining_y`，否则原量回传。
    auto on_scroll(ScrollEvent &e) -> void override {
        if (state_ == PullToRefreshState::Refreshing || e.delta_y <= 0.0F || !child_at_top()) {
            e.remaining_y = e.delta_y;  // 本容器不接：原量交还更浅层可滚动祖先
            return;
        }
        // 滚轮余量以「单位」计（全库约定 offset -= delta × step），换算成 dp 再进橡皮筋，
        // 否则滚轮通道要几百个单位才够阈值。缺省 step=16dp（`ScrollProps::step_default()` 同值）。
        const float grown = rubber(e.delta_y * 16.0F);
        if (grown <= 0.0F) {
            return;
        }
        pull_ = std::clamp(pull_ + grown, 0.0F, max_pull());
        state_ = PullToRefreshState::Pulling;
        e.is_handled = true;
        e.remaining_y = 0.0F;  // 余量已被下拉吃掉
        glide_.active = false;
        mark_needs_paint();
        request_frame(false);
    }

    /// @brief 本容器参与滚动派发（仅消费子级余量）。
    /// @return 恒为 true：滚轮派发时本控件是可滚动目标。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    /// @brief 指针拖拽通道：垂直下拉手势经橡皮筋映射为下拉距离；劫持期内吞掉事件，
    ///        松手按阈值裁决（触发刷新或回弹归零）；未劫持时修饰链照常下发子树。
    /// @param e 鼠标事件；劫持/刷新期置 `is_handled`。
    auto on_pointer_event(MouseEvent &e) -> void override {
        drag_.on_mouse(e);
        if (e.action == MouseAction::Press && child_at_top()) {
            drag_consumed_ = false;  // 顶部起按：开启下一次下拉候选
        }
        const bool vertical_down = drag_.axis() == DragAxis::Vertical && drag_.delta().y > 0.0F;
        if (e.action == MouseAction::Move && drag_.is_dragging() && vertical_down && !drag_consumed_ &&
            child_at_top() && state_ != PullToRefreshState::Refreshing) {
            pull_ = std::clamp(rubber(drag_.delta().y), 0.0F, max_pull());
            state_ = PullToRefreshState::Pulling;
            glide_.active = false;
            drag_consumed_ = true;  // 本手势归下拉所有：之后即使回落也不再劫持子级点击
            mark_needs_paint();
            request_frame(false);
        } else if (e.action == MouseAction::Move && drag_.is_dragging() && drag_consumed_) {
            // 已劫持手势：回落段继续跟手（允许拉回 0），反向超阈值时放弃劫持交还子级。
            if (drag_.delta().y <= 0.0F) {
                drag_consumed_ = false;
            } else {
                pull_ = std::clamp(rubber(drag_.delta().y), 0.0F, max_pull());
                mark_needs_paint();
                request_frame(false);
            }
        }
        if (e.action == MouseAction::Release && drag_consumed_) {
            drag_consumed_ = false;
            release_pull();
        }
        if (drag_consumed_ || state_ == PullToRefreshState::Refreshing) {
            e.is_handled = true;  // 下拉/刷新手势期消费指针事件，子级不误触点击
        } else {
            SingleChild::on_pointer_event(e);  // 未劫持：修饰链照常下发子树
        }
        if (e.action == MouseAction::Release) {
            drag_.reset();
        }
    }

    /// @brief 触摸通道：喂给拖拽识别器后原样下发子树（劫持裁决在 mouse 重载，逻辑同款）。
    /// @param e 触摸事件。
    auto on_pointer_event(TouchEvent &e) -> void override {
        drag_.on_touch(e);
        SingleChild::on_pointer_event(e);  // 修饰链照常下发子树
    }

  protected:
    /// @brief 视口透传布局：子树取本容器盒尺寸（滚动子树自管内滚偏移）。
    /// @param c 父级传入的布局约束。
    /// @param ctx 构建上下文（透传给子树）。
    /// @return 夹取后的容器尺寸（子树占满）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;  // 默认吃满父约束上限（视口透传）
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 480.0F};
        }
        if (child_) {
            const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        }
        return c.constrain(self);
    }

    /// @brief 子树之上叠加顶部下拉指示器带：半透明底 + 按进度扫过的弧线 spinner，刷新中附提示文案。
    /// @param p 目标画笔。
    /// @param bounds 本容器的绘制矩形（相对坐标系）。
    /// @param ctx 构建上下文（取主题色）。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        SingleChild::on_paint(p, bounds, ctx);  // 先绘制滚动子树（内容层）
        if (pull_ <= 0.4F) {
            return;
        }
        // 顶部覆盖层带：半透明底 + 旋转弧线 spinner（不动布局盒）。
        const Color accent = indicator_color(ctx);
        const Rect band{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y},
                        .size = Size{.width = bounds.size.width, .height = pull_}};
        const double prev_alpha = p.global_alpha();
        p.set_alpha(prev_alpha * 0.92);
        p.fill_rect(band, band_color(ctx));
        p.set_alpha(prev_alpha);
        const float angle = spin_angle_;
        p.stroke_arc(
            Point{.x = band.origin.x + (band.size.width / 2.0F), .y = band.origin.y + (band.size.height / 2.0F)}, 10.0F,
            2.5F, angle, angle + (4.712389F * std::clamp(progress(), 0.25F, 1.0F)), accent);
        if (state_ == PullToRefreshState::Refreshing) {
            const float th = render::FontEngine::measure_height(Font{.size_pt = 11.0F});
            // CJK-LITERAL: on-screen-demo - 刷新中 提示绘制在下拉带上，属上屏文案，不经控制台输出
            p.draw_text(Rect{.origin = Point{.x = band.origin.x, .y = band.origin.y + pull_ - th - 4.0F},
                             .size = Size{.width = band.size.width, .height = th}},
                        "刷新中…", Font{.size_pt = 11.0F}, refreshing_text_color(ctx));
        }
    }

    /// @brief 回弹/旋转逐帧推进（自驱动 tick，同 Scroll/Dismissible 模式）。
    ///        步长经 `scaled_frame_dt` 折算：应用内全局时间缩放对旋转指示器与回弹滑动同时生效，
    ///        冻结（倍率 0）时本帧既不推进也不标脏、不请求帧，但保持每帧计时武装。
    /// @param now 当前帧时刻（墙钟，用于计算帧间隔 dt）。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        SingleChild::tick_gestures(now);  // 先驱动子级手势
        const double raw =
            last_tick_.has_value() ? std::chrono::duration<double>(now - *last_tick_).count() : (1.0 / 60.0);
        last_tick_ = now;
        // 单一 dt 漏斗：缩放只在此处发生一次（`ScrollGlide::tick` 与旋转累加都吃折算后的步长）。
        const double dt = scaled_frame_dt(raw);
        if (dt <= 0.0) {
            // 冻结帧的空闲纪律：旋转与回弹都停在冻结前的姿态——不 mark_needs_paint、不 request_frame
            // （否则「停着不动却满帧唤醒」），也不摘 `needs_gesture_tick_`：解冻由
            // `Animator::take_rearm_request` 送一帧进来，本函数由此续跑，冻结前进度不丢。
            return;
        }
        bool busy = false;
        if (state_ == PullToRefreshState::Refreshing) {
            spin_angle_ += static_cast<float>(dt) * 6.0F;  // ~1 圈/秒
            if (spin_angle_ > 6.2831853F) {
                spin_angle_ -= 6.2831853F;
            }
            mark_needs_paint();
            request_frame(false);  // 旋转帧 = 活跃帧，防滑入空闲深睡
            busy = true;
        }
        if (glide_.active) {
            const float v = std::clamp(glide_.tick(dt), 0.0F, max_pull());
            if (!glide_.active) {
                needs_gesture_tick_ = false;
            }
            if (v != pull_) {
                pull_ = v;
                mark_needs_paint();
                request_frame(false);
            }
            busy = true;
        }
        if (!busy) {
            needs_gesture_tick_ = false;
            last_tick_.reset();
        }
    }

  private:
    /// @brief 橡皮筋（阻尼）映射：物理位移(dp) → 指示器位移(dp)。
    ///        双曲阻尼 `raw·max/(raw+max)`：起段近 1:1 跟手，渐近 `max_pull` 且恒不越界；
    ///        拖满 `max_pull` 恰达半上限 —— 与缺省 `threshold = max_pull/2` 对齐（可触发）。
    [[nodiscard]] auto rubber(float raw) const -> float {
        const float limited = std::max(0.0F, raw);
        const float limit = max_pull();
        if (limit <= 0.0F) {
            return 0.0F;
        }
        return std::min(limit, (limited * limit) / (limited + limit));
    }

    /// @brief 滚动子树是否位于顶部：找最近的可滚动后代（实现 `accessibility_scroll`），
    ///        position<=min 即顶部；无可滚动后代视为「在顶部」（静态内容可整体下拉）。
    [[nodiscard]] auto child_at_top() const -> bool {
        if (!child_) {
            return true;
        }
        const auto range = first_scrollable_of(child_.widget());
        return !range.has_value() || range->position <= range->min + 0.5;
    }

    static auto first_scrollable_of(const Widget &w) -> std::optional<AccessibilityScrollRange> {
        if (const auto range = w.accessibility_scroll(); range.has_value()) {
            return range;
        }
        std::optional<AccessibilityScrollRange> found;
        w.for_each_child([&found](const Widget &c) {
            if (!found.has_value()) {
                found = first_scrollable_of(c);
            }
        });
        return found;
    }

    /// @brief 松手裁决：进度达阈值 → 进入刷新态并回弹到驻留高度（约半阈值）；否则回弹归零。
    auto release_pull() -> void {
        if (pull_ >= threshold() && on_refresh_) {
            state_ = PullToRefreshState::Refreshing;
            needs_gesture_tick_ = true;  // spinner 逐帧旋转依赖 tick（回弹目标可能与当前值相等而早退）
            on_refresh_();
            begin_setback_glide(std::min(pull_, threshold() * 0.5F));
        } else {
            state_ = PullToRefreshState::Idle;
            begin_setback_glide(0.0F);
        }
    }

    /// @brief 启动回弹滑动；reduce-motion 下直落目标（无中间帧）。
    auto begin_setback_glide(float target) -> void {
        if (target == pull_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            glide_.active = false;
            pull_ = target;
            mark_needs_paint();
            request_frame(false);
            return;
        }
        glide_.start(pull_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    std::function<void()> on_refresh_;
    DragRecognizer drag_;
    bool drag_consumed_ = false;  ///< 本手势已被下拉劫持（子级点击不再参与）
    PullToRefreshState state_ = PullToRefreshState::Idle;
    float pull_ = 0.0F;  ///< 当前下拉距离（dp）
    float spin_angle_ = 0.0F;  ///< spinner 累计旋转角（弧度）
    ScrollGlide glide_;  ///< 回弹短滑动时序
    std::optional<std::chrono::steady_clock::time_point> last_tick_;
};

}  // namespace aurora