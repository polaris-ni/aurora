#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/environment/media_query.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 下拉选择器：点击展开选项列表，选择后收起。
///
/// 选项为字符串列表（序列化友好、AI 可枚举）；泛型选项经 `on_change(index)`
/// 回调在调用侧映射。选中序号存于响应式 `selected()`。
///
/// 可定制性（对标 Qt QComboBox / Flutter DropdownButton）：
/// - 颜色：主框背景/边框/文本/箭头；选中高亮 `accent_color` 未显式设置时跟随主题 `Theme::primary`；
/// - 尺寸：主框高（`set_box_height`）、选项行高（`set_item_height`）、字号、圆角；
/// - 空选项时显示 `placeholder`；禁用（`set_enabled(false)`）灰化并忽略点击。
///
/// 继承扩展点（protected 虚函数）：`paint_box`（主框）与 `paint_item`（单个下拉选项行）。
///
/// 对标 Qt `QComboBox`、WPF `ComboBox`、Flutter `DropdownButton`、SwiftUI `Picker`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// @note 本行隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_change_，而其拷贝与 operator()
/// 皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
/// 本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
/// 刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class Dropdown : public Widget {
  public:
    // 隐式默认构造平凡：无选项、选中序号 0、收起态，样式取成员声明处默认值（§13.5.1 豁免升格为文档）。
    Dropdown() = default;
    /// @brief 以选项列表与初始选中序号构造。
    /// @param options 选项文本列表；可为空（空时主框显示 placeholder）。
    /// @param initial 初始选中序号；越界时钳制到 [0, options.size()-1]，空列表取 0。
    explicit Dropdown(std::vector<std::string> options, int initial = 0) : options_(std::move(options)) {
        const int max_idx = static_cast<int>(options_.size()) - 1;
        selected_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    /// @brief 控件类型名，自描述/Inspector 据此分派工厂。
    /// @return 静态字符串 "Dropdown"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Dropdown"; }

    /// @brief Dropdown 的静态控件描述符：属性键、事件、不变量与示例（定义见类外 inline 实现）。
    /// @return 完整 WidgetDescriptor。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例侧描述入口，转发 describe_static()。
    /// @return 与 describe_static() 相同的 WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号。
    /// @param out 输出收集器；追加选中序号信号 selected_ 的视图指针。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&selected_); }

    /// @brief 选项数量。
    /// @return options_ 的大小。
    [[nodiscard]] auto option_count() const -> std::size_t { return options_.size(); }
    /// @brief 选中序号的响应式信号（可订阅/写入，越界值不在此钳制）。
    /// @return selected_ 的引用。
    [[nodiscard]] auto selected() -> State<int> & { return selected_; }
    /// @brief 当前选中序号快照。
    /// @return selected_ 的当前值。
    [[nodiscard]] auto selected_index() const -> int { return selected_.get(); }
    /// @brief 当前选中项的文本。
    /// @return 序号对应选项文本；序号为负或 >= option_count() 时返回空串。
    [[nodiscard]] auto selected_text() const -> std::string {
        const int i = selected_.get();
        if (i < 0 || std::cmp_greater_equal(i, options_.size())) {
            return {};
        }
        return options_[static_cast<std::size_t>(i)];
    }
    /// @brief 下拉面板是否处于展开态。
    /// @return 展开为 true。
    [[nodiscard]] auto is_open() const -> bool { return is_open_; }

    /// @brief 选择指定序号（越界忽略；触发 on_change）。
    /// @param index 目标选项序号；负值或 >= option_count() 时忽略，与当前值相同时不回调。
    auto select(int index) -> void {
        if (index >= 0 && std::cmp_less(index, options_.size()) && index != selected_.get()) {
            selected_.set(index);
            mark_needs_paint();
            if (on_change_) {
                on_change_(index);
            }
        }
    }

    /// @brief 展开/收起下拉。
    /// @param open true 展开选项面板，false 收起。
    auto set_open(bool open) -> void {
        is_open_ = open;
        mark_needs_paint();
    }

    /// @brief 设置选择回调（链式）。
    /// @param cb 新选中序号的通知回调（参数为序号）；置空则选中后不回调。
    /// @return 引用 *this。
    auto set_on_change(std::function<void(int)> cb) -> Dropdown & {
        on_change_ = std::move(cb);
        return *this;
    }

    /// @brief 设置空选项占位文本（链式）。
    /// @param s 选项列表为空时主框显示的文本。
    /// @return 引用 *this。
    auto set_placeholder(std::string s) -> Dropdown & {
        placeholder_ = std::move(s);
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选中高亮色（链式）。不调用则跟随主题 `Theme::primary`。
    /// @param c 选中项的强调色（面板淡底与文本用色）。
    /// @return 引用 *this。
    auto set_accent_color(Color c) -> Dropdown & {
        accent_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框背景色（链式）。
    /// @param c 主框与下拉面板的填充背景色。
    /// @return 引用 *this。
    auto set_box_color(Color c) -> Dropdown & {
        box_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框/面板边框色（链式）。
    /// @param c 常态边框色；悬停/展开时边框改绘为强调色。
    /// @return 引用 *this。
    auto set_border_color(Color c) -> Dropdown & {
        border_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置选项文本色（链式）。
    /// @param c 主框选中文本与非选中选项行的文本色。
    /// @return 引用 *this。
    auto set_text_color(Color c) -> Dropdown & {
        text_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置箭头颜色（链式）。
    /// @param c 主框右侧开合箭头（v/^）的绘制色。
    /// @return 引用 *this。
    auto set_arrow_color(Color c) -> Dropdown & {
        arrow_color_ = c;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置主框高度 dp（链式）。
    /// @param h 主框高度（dp）；<= 0 时回退默认 30.0。
    /// @return 引用 *this。
    auto set_box_height(float h) -> Dropdown & {
        box_height_ = h > 0.0F ? h : 30.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置选项行高 dp（链式）。
    /// @param h 下拉面板单行选项高度（dp）；<= 0 时回退默认 26.0。
    /// @return 引用 *this。
    auto set_item_height(float h) -> Dropdown & {
        item_height_ = h > 0.0F ? h : 26.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置字号 pt（链式）。
    /// @param s 主框与选项行文本字号（pt）；<= 0 时回退默认 13.0。
    /// @return 引用 *this。
    auto set_font_size(float s) -> Dropdown & {
        font_size_ = s > 0.0F ? s : 13.0F;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置主框圆角半径 dp（链式；0 = 直角）。
    /// @param r 主框圆角半径（dp）；负值钳为 0（直角绘制）。
    /// @return 引用 *this。
    auto set_corner_radius(float r) -> Dropdown & {
        corner_radius_ = r >= 0.0F ? r : 0.0F;
        mark_needs_paint();
        return *this;
    }

    /// @brief 设置是否启用（链式）；禁用态灰化绘制并忽略点击。
    /// @param v true 可交互；false 进入禁用态（统一灰化配色，点击被吞掉不冒泡）。
    /// @return 引用 *this。
    auto set_enabled(bool v) -> Dropdown & {
        enabled_ = v;
        mark_needs_paint();
        return *this;
    }
    /// @brief 当前启用状态。
    /// @return 启用为 true。
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }

    /// @brief 点击交互：框内点击开合；展开时点击可视窗口内的选项选中并收起。
    /// @param e 鼠标事件；仅 Press 参与判定，其余动作转发基类。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!enabled_) {
            e.is_handled = true;  // 禁用态吞掉点击（不冒泡），不开合
            return;
        }
        if (e.action != MouseAction::Press) {
            Widget::on_pointer_event(e);
            return;
        }
        // 主框区域：开合切换
        if (e.local_position.y < box_height_) {
            is_open_ = !is_open_;
            // 每次展开从顶部开始：否则上次滚动到的位置会让「展开后首屏」随上次状态漂移。
            panel_scroll_ = 0.0F;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        // 展开中的选项列表：序号由「可视窗口 + 滚动偏移」反算，与 panel_geometry 同一份算式。
        // 只认落在可视窗口内的点（翻转时窗口在主框上方，故此处不按 local.y >= box_height_ 分流）。
        // self_top 由事件自带的两份坐标反算：`position`（全局，命中链冒泡时不变）减
        // `local_position`（本控件本地，派发器逐控件改写）＝ 本控件原点的全局 y。
        // 事件路径因此完全不需要 BuildContext 或绘制期缓存。
        const float self_top = e.position.y - e.local_position.y;
        const std::optional<PanelGeometry> geo = panel_geometry(size().width, self_top);
        if (geo.has_value() && geo->window.contains(e.local_position)) {
            const float content_y = (e.local_position.y - geo->window.origin.y) + geo->scroll;
            const int idx = static_cast<int>(std::floor(content_y / item_height_));
            if (idx >= 0 && std::cmp_less(idx, options_.size())) {
                select(idx);
            }
            is_open_ = false;
            panel_scroll_ = 0.0F;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 挂载时存环境链：布局与绘制都还没跑的首帧也能读到视口高。
    ///
    /// `on_paint` 是主写入点（每帧必过），`on_layout` 是兜底（**布局缓存命中时整段被跳过**，
    /// 见 `Widget::layout`），本入口则保证「一次都没布局/绘制过」时也有环境链可用。
    /// @param ctx 构建上下文：取其环境链地址（`root_env_`，窗口生命周期内恒定）。
    auto on_mount(const BuildContext &ctx) -> void override { env_ = ctx.env; }

    /// @brief 清空缓存的环境链地址（与 `on_mount` 对称）。
    ///
    /// `env_` 指向宿主的环境链，被摘下后宿主可能先销毁 ⇒ 留着就是悬垂指针。下次布局的兜底写入
    /// （`on_layout` / `on_paint`）会重新填上，故清空不影响任何读取路径。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        env_ = nullptr;
    }

    /// @brief 声明本控件参与点击分发。
    /// @return 恒为 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 面板被限高时本控件成为滚轮派发目标（面板内部滚动）。
    ///
    /// 派发器沿命中链自最深向根找第一个 `wants_scroll()` 者，故须在**展开且内容超出
    /// 可视窗口**时才为真——否则收起态 / 短列表会白吃掉滚轮，外层列表/页面就滚不动了。
    /// @return 展开态且面板内容高超出可视窗口高时为 true。
    [[nodiscard]] auto wants_scroll() const -> bool override {
        const std::optional<PanelGeometry> geo = panel_geometry(size().width, focus_bounds_.origin.y);
        return geo.has_value() && geo->content_h > geo->window.size.height + 1e-3F;
    }

    /// @brief 追加命中盒：展开态的下拉面板（画在主框下方或上方、自身布局盒之外）。
    ///
    /// 面板是覆盖绘制：它不占布局，祖先下降前的包含闸只看布局盒，故面板区域必须在此
    /// 声明才会被闸并入命中链——判据写在 `on_hit_test` 里对真实派发无效（派发只走
    /// `on_hit_test_chain`）。与 `on_hit_test` 的面板分支共用 `panel_box()` 这一份判据。
    /// 声明的是**可视窗口盒**（翻转 + 限高后的那块），未限高的部分须经面板内滚动才可达。
    /// @param ancestor_offset 祖先下降时累加传入的本控件全局原点；其 `.y` 即翻转判据所需的
    ///        「离视口多远」，故本控件的追加盒几何**依赖**它（其余覆写方忽略即可）。
    /// @return 展开态为面板可视窗口矩形（本地坐标）；收起态为 `std::nullopt`（可命中区 == 主框）。
    [[nodiscard]] auto extra_hit_box(const BuildContext & /*ctx*/, const Point &ancestor_offset) const
        -> std::optional<Rect> override {
        return panel_box(size().width, ancestor_offset.y);
    }

    /// @brief 悬停反馈：主框边框高亮为强调色。
    /// @param entered true 悬停进入，false 悬停离开。
    auto on_hover_change(bool entered) -> void override {
        Widget::on_hover_change(entered);
        if (enabled_) {
            mark_needs_paint();
        }
    }

    /// @brief 序列化全部可定制属性与选中态到 props。
    /// @param props 输出 JSON 对象：options/selected_index/placeholder/各颜色/尺寸/enabled；
    ///        accent_color 未显式设置时不输出，保留「跟随主题」语义。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        Json opts = Json::array();
        for (const auto &o : options_) {
            opts.push_back(o);
        }
        props.set("options", opts);
        props.set("selected_index", selected_.get());
        if (!placeholder_.empty()) {
            props.set("placeholder", placeholder_);
        }
        if (accent_color_.has_value()) {
            props.set("accent_color", color_to_json(*accent_color_));  // 未设置不输出：保留「跟随主题」语义
        }
        props.set("box_color", color_to_json(box_color_));
        props.set("border_color", color_to_json(border_color_));
        props.set("text_color", color_to_json(text_color_));
        props.set("arrow_color", color_to_json(arrow_color_));
        props.set("box_height", box_height_);
        props.set("item_height", item_height_);
        props.set("font_size", font_size_);
        props.set("corner_radius", corner_radius_);
        props.set("enabled", Json{enabled_});
    }

    /// @brief 从 props 回填属性，与 serialize_props 对称；缺失的键保持现值不变。
    /// @param props 序列化产物 JSON 对象（options/selected_index/样式各键）。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        const auto *options_val = props.at("options");
        if (options_val != nullptr && options_val->is_array()) {
            options_.clear();
            for (const auto &o : *options_val) {
                options_.push_back(o.as_or<std::string>(""));
            }
        }
        if (props.contains("selected_index")) {
            selected_.set(props.at("selected_index")->as_or<std::int32_t>(0));
        }
        if (props.contains("placeholder")) {
            placeholder_ = props.at("placeholder")->as_or<std::string>("");
        }
        if (props.contains("accent_color")) {
            accent_color_ = json_to_color(*props.at("accent_color"));
        }
        if (props.contains("box_color")) {
            box_color_ = json_to_color(*props.at("box_color"));
        }
        if (props.contains("border_color")) {
            border_color_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("text_color")) {
            text_color_ = json_to_color(*props.at("text_color"));
        }
        if (props.contains("arrow_color")) {
            arrow_color_ = json_to_color(*props.at("arrow_color"));
        }
        if (props.contains("box_height")) {
            box_height_ = props.at("box_height")->as_or<float>(0.0F);
        }
        if (props.contains("item_height")) {
            item_height_ = props.at("item_height")->as_or<float>(0.0F);
        }
        if (props.contains("font_size")) {
            font_size_ = props.at("font_size")->as_or<float>(0.0F);
        }
        if (props.contains("corner_radius")) {
            corner_radius_ = props.at("corner_radius")->as_or<float>(0.0F);
        }
        if (props.contains("enabled")) {
            enabled_ = props.at("enabled")->as_or<bool>(false);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        env_ = ctx.env;  // 环境链兜底写入（主写入点在 on_paint，见 PanelGeometry 文档）
        // 主框宽度 = 最长选项宽 + 箭头区；下拉为覆盖绘制不占布局
        Font f;
        f.size_pt = font_size_;
        float w = 80.0F;
        for (const auto &o : options_) {
            w = std::max(w, render::FontEngine::measure_width(o, f) + (AURORA_PAD * 2.0F) + AURORA_ARROW_ZONE);
        }
        if (options_.empty() && !placeholder_.empty()) {
            w = std::max(w,
                         render::FontEngine::measure_width(placeholder_, f) + (AURORA_PAD * 2.0F) + AURORA_ARROW_ZONE);
        }
        return c.constrain(Size{.width = w, .height = box_height_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 环境链主写入点：每帧必过、不受布局缓存影响（on_layout 在缓存命中时被整段跳过，
        // 不可作为唯一写入点）。存的是 root_env_ 的地址——窗口生命周期内恒定、每帧原地
        // 更新，故事件侧查链读到的视口高恒新鲜，不像「采样成标量」那样滞后一帧。
        env_ = ctx.env;
        Font f;
        f.size_pt = font_size_;
        // 状态色解析：显式设置优先，否则跟随主题 primary；禁用态统一灰化。
        Color accent = accent_color_.value_or(inherit_theme(ctx).primary);
        Color box = box_color_;
        Color border = border_color_;
        Color text = text_color_;
        Color arrow = arrow_color_;
        if (!enabled_) {
            accent = Color{176, 176, 180, 255};
            box = Color{235, 235, 237, 255};
            border = Color{215, 215, 219, 255};
            text = Color{168, 168, 172, 255};
            arrow = Color{190, 190, 194, 255};
        } else if (hovered() || is_open_) {
            border = accent;  // 悬停/展开时边框高亮
        }

        const Rect box_rect{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = box_height_}};
        paint_box(p, box_rect, f, box, border, text, arrow);

        // 下拉选项面板：几何与命中同源（panel_geometry 一份算式），翻转 / 限高 / 滚动偏移
        // 三者在绘制与判定两侧逐字一致，否则出现「看得见的行点不到」或反之。
        if (const std::optional<PanelGeometry> geo = panel_geometry(box_rect.size.width, bounds.origin.y);
            geo.has_value()) {
            const Rect drop = geo->window;
            p.draw_shadow(drop, 0.0F, 2.0F, 8.0F, Color(0, 0, 0, 48));
            p.fill_rect(drop, box_color_);
            p.draw_rect(drop, border_color_);
            // 裁剪到窗口盒：被限高时只画窗口内那几行，其余行滚入后才可见。
            p.push_clip(drop);
            const float y = drop.origin.y - geo->scroll;  // 内容顶相对窗口顶上移 scroll
            for (std::size_t i = 0; i < options_.size(); ++i) {
                const Rect item{.origin = Point{.x = drop.origin.x, .y = y + (static_cast<float>(i) * item_height_)},
                                .size = Size{.width = drop.size.width, .height = item_height_}};
                paint_item(p, i, item, std::cmp_equal(i, selected_.get()), f, accent, text);
            }
            p.pop_clip();
        }
    }

    /// @brief 继承扩展点：绘制主框（背景/边框/选中文本/箭头）。
    /// @param p 绘制器。
    /// @param box 主框矩形。
    /// @param f 文本字体。
    /// @param bg 背景填充色。
    /// @param border 边框色。
    /// @param text 选中文本色。
    /// @param arrow 箭头色。
    virtual auto paint_box(Painter &p, const Rect &box, const Font &f, Color bg, Color border, Color text, Color arrow)
        -> void {
        if (corner_radius_ > 0.0F) {
            p.fill_rounded_rect(box, corner_radius_, bg);
            p.draw_rounded_border(box, corner_radius_, 1.0F, border);
        } else {
            p.fill_rect(box, bg);
            p.draw_rect(box, border);
        }
        const std::string shown = options_.empty() ? placeholder_ : selected_text();
        const Rect text_box{
            .origin = Point{.x = box.origin.x + AURORA_PAD, .y = box.origin.y + ((box.size.height - 14.0F) * 0.5F)},
            .size = Size{.width = box.size.width - (AURORA_PAD * 2.0F) - AURORA_ARROW_ZONE, .height = 14.0F}};
        p.draw_text(text_box, shown, f, text);
        // 箭头（简化为 "v"/"^"）
        const Rect arrow_box{.origin = Point{.x = box.origin.x + box.size.width - AURORA_ARROW_ZONE,
                                             .y = box.origin.y + ((box.size.height - 14.0F) * 0.5F)},
                             .size = Size{.width = AURORA_ARROW_ZONE - 4.0F, .height = 14.0F}};
        p.draw_text(arrow_box, is_open_ ? "^" : "v", f, arrow);
    }

    /// @brief 继承扩展点：绘制单个下拉选项行（选中项铺强调色淡底 + 强调色文本）。
    /// @param p 绘制器。
    /// @param index 选项下标（取 options_[index] 文本）。
    /// @param item 该选项行矩形。
    /// @param selected 该行是否选中（选中铺淡底并用强调色文本）。
    /// @param f 文本字体。
    /// @param accent 强调色（选中行底色按 alpha 淡化）。
    /// @param text 未选中项文本色。
    virtual auto paint_item(Painter &p, std::size_t index, const Rect &item, bool selected, const Font &f, Color accent,
                            Color text) -> void {
        if (selected) {
            p.fill_rect(item, accent.with_alpha(30));
        }
        const Rect item_box{
            .origin = Point{.x = item.origin.x + AURORA_PAD, .y = item.origin.y + 5.0F},
            .size = Size{.width = item.size.width - (AURORA_PAD * 2.0F), .height = item.size.height - 10.0F}};
        p.draw_text(item_box, options_[index], f, selected ? accent : text);
    }

    /// @brief 下拉面板几何：翻转 / 高度上限 / 内部滚动的**唯一算式**，四处共用一份。
    ///
    /// 面板是覆盖绘制，既无上限也不滚动时，档位一多（真实场景：系统等宽字体枚举 20+ 档）
    /// 会直接伸出窗口外，尾部既不可见也不可点。本结构把「可视窗口」与「完整内容」拆成两个盒：
    /// - `window`：实际绘制与命中的那块（局部坐标），受视口高度上限约束，位置随翻转而变；
    /// - `content_h`：全部选项的完整高度（dp），可超出 `window.size.height`；
    /// - `scroll`：内容相对窗口的滚动偏移（dp，恒被夹取在 `[0, content_h - window.h]`）。
    ///
    /// 翻转规则：下方剩余空间装不下时，若上方装得下则整体翻到主框上方（`window.origin.y` 为负）；
    /// 两侧都装不下时，取空间较大的一侧并把 `window` 限高——此时内容需内部滚动才可达尾部。
    ///
    /// **视口语义（两种，注入那份为准）**：`viewport_h` 读「最近祖先 `MediaQuery` Provider 注入的
    /// 视口逻辑高」——链上无 Provider 时由 `Window::prepare_context` 把 `MediaQuery::from_surface`
    /// 注入 `root_env_`，那一份即**第一真值源**（等价于 `Window::size()` ← `Surface::size()`）；
    /// 应用经 `Provider<MediaQuery>` 显式注入的子树视口作为**可选覆写**（适用于把子树布局在
    /// 嵌入区、面板应贴合该区而非整窗的场景）。
    ///
    /// **两个几何读数各走各的通道，都不依赖绘制期缓存**：
    /// - `viewport_h`：查 `env_` 环境链上最近祖先注入的 `MediaQuery`（`on_layout` 存的 `root_env_`
    ///   地址，窗口期恒定、每帧原地更新）⇒ 恒新鲜。注意不能只写在 `on_layout`：`Widget::layout`
    ///   在**布局缓存命中时直接 return、完全跳过 `on_layout`**，故纯 on_layout 写入不可靠。
    /// - `self_top`（本控件全局顶边 y）：命中链下降时由祖先逐层累加传入（`extra_hit_box` 的
    ///   `ancestor_offset` 形参）；事件路径另有一路——`MouseEvent::position`（全局）减
    ///   `local_position`（本地）即得，无需任何缓存。`on_paint` 用 `bounds.origin`。
    ///
    /// 绘制与判定共用本函数，故两侧**永远**同源；`self_top` 在「无事件坐标」的入口
    /// （`wants_scroll` / `on_scroll`）退化为 `focus_bounds_.origin.y`，而那两处只判
    /// 「是否可滚」不判位置，故退化无害。
    struct PanelGeometry {
        Rect window;  ///< 可视窗口盒（本地坐标）：绘制与命中的实际区域
        float content_h = 0.0F;  ///< 完整内容高（dp）：`option_count() * item_height()`
        float scroll = 0.0F;  ///< 内部滚动偏移（dp）：内容顶相对窗口顶的位移
    };

    /// @brief 面板可视窗口盒（本地坐标）：`extra_hit_box()` / `on_hit_test()` 的唯一来源。
    /// @param width 面板宽度（取主框宽度）。
    /// @param self_top 本控件原点在视口坐标系中的 y（祖先下降时累加传入）。
    /// @return 展开态为可视窗口盒；收起态为 `std::nullopt`。
    [[nodiscard]] auto panel_box(float width, float self_top) const -> std::optional<Rect> {
        const std::optional<PanelGeometry> geo = panel_geometry(width, self_top);
        return geo.has_value() ? std::optional<Rect>{geo->window} : std::nullopt;
    }

    /// @brief 视口逻辑高（dp）：查 `env_` 环境链上最近祖先注入的 `MediaQuery`。
    /// @return 视口高；环境链无 `MediaQuery` 或读数为非正时返回 0（调用方据此退化为无限高）。
    [[nodiscard]] auto viewport_height() const -> float {
        const MediaQuery *mq = (env_ != nullptr) ? env_->get<MediaQuery>() : nullptr;
        return (mq != nullptr) ? mq->size.height : 0.0F;
    }

    /// @brief 面板几何（翻转 + 限高 + 滚动偏移）的唯一实现；绘制与全部命中入口共用它。
    /// @param width 面板宽度（取主框宽度）。
    /// @param self_top 本控件原点在视口坐标系中的 y（祖先下降时累加传入）。
    /// @return 展开态为面板几何；收起态为 `std::nullopt`。
    [[nodiscard]] auto panel_geometry(float width, float self_top) const -> std::optional<PanelGeometry> {
        if (!is_open_) {
            return std::nullopt;
        }
        const float full_h = static_cast<float>(options_.size()) * item_height_;
        const float viewport_h = viewport_height();
        if (viewport_h <= 0.0F) {
            // 无视口读数（未布局 / 环境链无 MediaQuery）：退化为无限高、恒向下、不可滚。
            return PanelGeometry{.window = Rect{.origin = Point{.x = 0.0F, .y = box_height_},
                                                .size = Size{.width = width, .height = full_h}},
                                 .content_h = full_h,
                                 .scroll = 0.0F};
        }
        // 主框下沿 / 上沿到视口边界的可用空间（self_top 由祖先下降时累加传入，见 extra_hit_box）。
        const float below = viewport_h - (self_top + box_height_);
        const float above = self_top;
        const float limit = std::max(viewport_h - (2.0F * AURORA_PANEL_MARGIN), AURORA_MIN_PANEL_HEIGHT);

        float origin_y = box_height_;  // 默认向下：紧贴主框下沿
        float window_h = full_h;
        if (full_h > below) {
            // 下方装不下：上方装得下则整体上翻，否则取较大一侧并限高（内容内部滚动）。
            if (above >= full_h) {
                origin_y = box_height_ - full_h;  // 上翻：窗口顶落在主框上方（负值）
            } else {
                window_h = std::min((above > below) ? above : below, limit);
            }
        }
        window_h = std::min(window_h, limit);
        const float max_scroll = std::max(0.0F, full_h - window_h);
        return PanelGeometry{
            .window = Rect{.origin = Point{.x = 0.0F, .y = origin_y}, .size = Size{.width = width, .height = window_h}},
            .content_h = full_h,
            .scroll = std::clamp(panel_scroll_, 0.0F, max_scroll)};
    }

    /// @brief 命中测试：主框区域与展开中的下拉面板命中本控件，其余区域不命中。
    ///
    /// 「自身盒」分支必须与祖先下降闸同一谓词（`Rect{0,0,bounds.size}.contains(local)`）——
    /// 曾额外加 `local.y < box_height_` 这类**严格**比较，而闸用矩形的闭区间包含判定，
    /// 两者在边界行（y == box_height_）分叉：派发入口命中、兼容入口不命中。
    /// @param local 相对本控件左上角的命中点坐标。
    /// @param bounds 本控件布局后的矩形。
    /// @return 命中时返回 this，未命中返回 nullptr。
    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override {
        // 自身盒（与闸同谓词）
        if (Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)) {
            return this;
        }
        // 展开的下拉区：与追加命中盒同一份判据（面板画在布局盒外，不要求点落在自身布局盒内）。
        if (const std::optional<Rect> drop = panel_box(bounds.size.width, bounds.origin.y);
            drop.has_value() && drop->contains(local)) {
            return this;
        }
        return nullptr;
    }

    /// @brief 滚轮：面板被限高时把余量转成面板内部滚动，吃不完的上冒给外层可滚动祖先。
    /// @param e 滚轮事件：读 `delta_y`，回写 `is_handled` 与未吸收余量 `remaining_y`。
    auto on_scroll(ScrollEvent &e) -> void override {
        // 面板未被限高（无需内部滚动）时不接管，交给外层——与「面板即完整内容」的情形一致。
        const std::optional<PanelGeometry> geo = panel_geometry(size().width, focus_bounds_.origin.y);
        const bool scrollable = geo.has_value() && geo->content_h > geo->window.size.height + 1e-3F;
        if (!scrollable || e.delta_y == 0.0F) {
            Widget::on_scroll(e);
            return;
        }
        e.is_handled = true;
        const float before = panel_scroll_;
        const float max_scroll = std::max(0.0F, geo->content_h - geo->window.size.height);
        panel_scroll_ = std::clamp(panel_scroll_ - e.delta_y, 0.0F, max_scroll);
        if (panel_scroll_ != before) {
            mark_needs_paint();
        }
        e.remaining_y = e.delta_y + (panel_scroll_ - before);
    }

    static constexpr float AURORA_PAD = 10.0F;  ///< 文本内边距(dp)
    static constexpr float AURORA_ARROW_ZONE = 24.0F;  ///< 箭头区宽度(dp)
    static constexpr float AURORA_PANEL_MARGIN = 8.0F;  ///< 面板与视口上下沿的最小留白(dp)
    static constexpr float AURORA_MIN_PANEL_HEIGHT = 48.0F;  ///< 限高后的最小可视面板高(dp)

    // NOLINTBEGIN(*-non-private-member-variables-in-classes)
    /// @brief 选项文本列表：构造/反序列化回填，绘制、命中测试与宽度测量按序号索引。
    std::vector<std::string> options_;
    State<int> selected_{0};  ///< 选中序号信号：写入即通知订阅者（collect_signals 暴露）
    bool is_open_ = false;  ///< 下拉面板展开态标记
    std::function<void(int)> on_change_;  ///< 选中变更回调（入参为新序号）；空 = 不回调
    std::string placeholder_;  ///< 空选项时的占位文本
    std::optional<Color> accent_color_;  ///< 选中高亮色；空 = 跟随主题 primary
    Color box_color_ = Color{255, 255, 255, 255};  ///< 主框背景色
    Color border_color_ = Color{200, 200, 205, 255};  ///< 主框/面板边框色
    Color text_color_ = Color{30, 30, 30, 255};  ///< 选项文本色
    Color arrow_color_ = Color{120, 120, 125, 255};  ///< 箭头颜色
    float box_height_ = 30.0F;  ///< 主框高度 dp
    float item_height_ = 26.0F;  ///< 选项行高 dp
    float panel_scroll_ = 0.0F;  ///< 面板内部滚动偏移 dp（仅面板被限高时非零；见 panel_geometry）
    const Environment *env_ = nullptr;  ///< 环境链（on_layout 从 ctx.env 存；窗口期恒定，事件阶段读视口用）
    float font_size_ = 13.0F;  ///< 字号 pt
    float corner_radius_ = 4.0F;  ///< 主框圆角半径 dp；0 = 直角
    bool enabled_ = true;  ///< 禁用态灰化并忽略点击
    // NOLINTEND(*-non-private-member-variables-in-classes)
};

/// @brief Dropdown 静态描述符定义：属性键（options/selected_index/样式与尺寸各键）、on_change
/// 事件、无孩子策略、选中序号不变量与构造示例，供 Inspector 与代码生成消费。
/// @return 填充完毕的 WidgetDescriptor。
inline auto Dropdown::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Dropdown",
        .properties =
            {
                {.name = "options",
                 .type = "vector<string>",
                 .default_value = "[]",
                 .required = true,
                 .note = "Option list",
                 .json_type = "array"},
                {.name = "selected_index",
                 .type = "int",
                 .default_value = "0",
                 .required = false,
                 .note = "Current selected index",
                 .json_type = "integer",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "placeholder",
                 .type = "string",
                 .default_value = "\"\"",
                 .required = false,
                 .note = "Placeholder text when nothing is selected",
                 .json_type = "string"},
                {.name = "accent_color",
                 .type = "Color",
                 .default_value = "theme.primary",
                 .required = false,
                 .note = "Selection highlight color (defaults to theme primary)",
                 .json_type = "array"},
                {.name = "box_color",
                 .type = "Color",
                 .default_value = "Color::white()",
                 .required = false,
                 .note = "Main box background color",
                 .json_type = "array"},
                {.name = "border_color",
                 .type = "Color",
                 .default_value = "{200,200,205,255}",
                 .required = false,
                 .note = "Main box / panel border color",
                 .json_type = "array"},
                {.name = "text_color",
                 .type = "Color",
                 .default_value = "{30,30,30,255}",
                 .required = false,
                 .note = "Option text color",
                 .json_type = "array"},
                {.name = "arrow_color",
                 .type = "Color",
                 .default_value = "{120,120,125,255}",
                 .required = false,
                 .note = "Arrow color",
                 .json_type = "array"},
                {.name = "box_height",
                 .type = "float",
                 .default_value = "30.0",
                 .required = false,
                 .note = "Main box height (dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "item_height",
                 .type = "float",
                 .default_value = "26.0",
                 .required = false,
                 .note = "Option row height (dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "font_size",
                 .type = "float",
                 .default_value = "13.0",
                 .required = false,
                 .note = "Font size (pt)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "corner_radius",
                 .type = "float",
                 .default_value = "4.0",
                 .required = false,
                 .note = "Main box corner radius (dp)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "enabled",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "Interactive (greyed out when disabled)",
                 .json_type = "boolean"},
            },
        .events = {"on_change"},
        .children_policy = "none",
        .invariants = {"selected_index >= 0", "selected_index < options.size()"},
        .examples = {R"(au::Dropdown({"Small", "Medium", "Large"}, 1))"},
    };
}
}  // namespace aurora
