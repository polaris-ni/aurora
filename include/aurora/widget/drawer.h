#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/event/focus.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 抽屉停靠侧：面板贴在窗口的左缘或右缘（`Left` 自 x=0 起算，`Right` 贴右边界）。
enum class DrawerSide : std::uint8_t { Left, Right };

/// @brief 抽屉/侧边栏：可滑出的侧边面板。
///
/// 两子结构：基础内容（占满）+ 抽屉面板（左/右停靠）。打开时绘制半透明遮罩
/// （模态语义），点击遮罩关闭；`permanent` 模式下面板始终可见、无遮罩、
/// 基础内容让出面板宽度。
///
/// 对标 Flutter `Drawer`/`EndDrawer`、Qt `QDockWidget`、SwiftUI `NavigationSplitView`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// @note 本类隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_toggle_，而其拷贝与 operator()
///       皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
///       本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
///       刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class Drawer : public Widget {
  public:
    /// @brief 默认构造：空内容/空面板、关闭态、左停靠、面板宽 240dp、非永久模式。
    Drawer() = default;
    /// @brief 以基础内容与抽屉面板构造。
    /// @param content 占满区域的子节点（永久模式下让出面板宽度），可为空 Node。
    /// @param panel 停靠面板子节点（打开或永久时参与布局/绘制），可为空 Node。
    /// @param side 停靠侧，默认 `DrawerSide::Left`。
    /// @param panel_width 面板宽度 dp；<= 0 时回落为默认 240dp。
    Drawer(Node content, Node panel, DrawerSide side = DrawerSide::Left, float panel_width = 240.0F)
        : content_(std::move(content)), panel_(std::move(panel)), side_(side),
          panel_width_(panel_width > 0.0F ? panel_width : 240.0F) {}

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"Drawer"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "Drawer"; }

    /// @brief 运行时自描述：open/side/panel_width/permanent 属性键、`on_toggle` 事件、多子节点策略。
    /// @return Drawer 的控件描述符（实现见 `src/aurora/widget/drawer.cpp`）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    /// @brief 运行时多态自描述入口。
    /// @return 转发 `describe_static()` 的结果。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集响应式信号：仅开合状态 `open_`（面板显隐与布局均由它驱动）。
    /// @param out 输出向量：挂载时由基类登记依赖。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&open_); }

    /// @brief 当前是否打开（永久模式恒返回构造/序列化所得值，不参与开合）。
    /// @return `open_` 信号的当前值。
    [[nodiscard]] auto is_open() const -> bool { return open_.get(); }
    /// @brief 取开合状态信号本体，供外部双向绑定/订阅。
    /// @return 内部 `State<bool>` 引用（true = 打开）。
    [[nodiscard]] auto open_state() -> State<bool> & { return open_; }
    /// @brief 是否处于永久模式（面板始终可见、无遮罩）。
    /// @return 永久模式标志。
    [[nodiscard]] auto is_permanent() const -> bool { return permanent_; }
    /// @brief 面板宽度 dp（构造时已把非正值归一为 240）。
    /// @return 期望面板宽度；实际布局宽度另受本控件宽度截断。
    [[nodiscard]] auto panel_width() const -> float { return panel_width_; }

    /// @brief 设置开合状态；仅在值真的变化时接线焦点作用域、标脏并回调 `on_toggle`。
    /// @param v 目标开合态；永久模式下直接忽略（无开合语义）。
    auto set_open(bool v) -> void {
        if (permanent_) {
            return;  // 永久模式无开合
        }
        if (v != open_.get()) {
            // 模态开合接线焦点作用域：打开压栈（Tab 关在抽屉面板内、焦点移入）、关闭弹栈恢复。
            if (current_focus_manager() != nullptr) {
                if (v) {
                    current_focus_manager()->push_scope(this);
                } else {
                    current_focus_manager()->pop_scope();
                }
            }
            open_.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (on_toggle_) {
                on_toggle_(v);
            }
        }
    }
    /// @brief 翻转开合状态（等价于 `set_open(!is_open())`）。
    auto toggle() -> void { set_open(!open_.get()); }

    /// @brief 永久模式（链式）：面板始终可见、无遮罩、内容让位。
    /// @param v true 进入永久模式（`set_open` 此后无效），false 回到可开合模态。
    /// @return 自身引用，便于链式调用。
    auto set_permanent(bool v) -> Drawer & {
        permanent_ = v;
        mark_needs_layout();
        return *this;
    }

    /// @brief 设置开合回调（链式）。
    /// @param cb 新回调，可空；仅在 `set_open` 真正改变状态时以新状态调用。
    /// @return 自身引用，便于链式调用。
    auto set_on_toggle(std::function<void(bool)> cb) -> Drawer & {
        on_toggle_ = std::move(cb);
        return *this;
    }

    /// @brief 模态打开时点击遮罩（面板外）关闭。
    /// @param e 鼠标事件；命中遮罩的 Press 会关闭抽屉并置 `is_handled`，其余下沉基类。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!permanent_ && open_.get() && e.action == MouseAction::Press) {
            const Rect panel = panel_rect();
            if (!panel.contains(e.local_position)) {
                set_open(false);
                e.is_handled = true;
                return;
            }
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 是否声明点击意图（决定事件派发是否把本控件纳入点击命中链）。
    /// @return 非永久模式且处于打开态时为 true（需拦截遮罩点击）。
    [[nodiscard]] auto wants_click() const -> bool override {
        return !permanent_ && open_.get();  // 打开时拦截遮罩点击
    }

    /// @brief 面板矩形（本控件局部坐标）。
    /// @return 宽度为 `min(panel_width, 控件宽)` 的竖条：左停靠自 x=0 起，右停靠贴右边界。
    [[nodiscard]] auto panel_rect() const -> Rect {
        const float w = std::min(panel_width_, size_.width);
        return side_ == DrawerSide::Left
                   ? Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = size_.height}}
                   : Rect{.origin = Point{.x = size_.width - w, .y = 0.0F},
                          .size = Size{.width = w, .height = size_.height}};
    }

    /// @brief 序列化自有属性：open、side（`left`/`right` 字面串）、panel_width、permanent。
    /// @param props 输出对象，先由基类填通用属性。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("open", Json{open_.get()});
        props.set("side", side_ == DrawerSide::Left ? "left" : "right");
        props.set("panel_width", panel_width_);
        props.set("permanent", Json{permanent_});
    }

    /// @brief 从 props JSON 还原自有属性（缺失键保持当前值）。
    /// @param props 属性 JSON；`side` 取 `right` 以外的一切值都按左停靠处理。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            open_.set(props.at("open")->as_or<bool>(false));
        }
        if (props.contains("side")) {
            side_ = props.at("side")->as_or<std::string>("") == "right" ? DrawerSide::Right : DrawerSide::Left;
        }
        if (props.contains("panel_width")) {
            panel_width_ = props.at("panel_width")->as_or<float>(0.0F);
        }
        if (props.contains("permanent")) {
            permanent_ = props.at("permanent")->as_or<bool>(false);
        }
    }

    /// @brief 遍历子节点：基础内容与面板各一次（空 Node 跳过）。
    /// @param fn 对每个存在子节点调用的回调（以子 widget 常量引用为参）。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (content_) {
            fn(content_.widget());
        }
        if (panel_) {
            fn(panel_.widget());
        }
    }

    /// @brief 子节点列表：按「内容 → 面板」顺序重建视图缓存（未设置的 Node 不入列）。
    /// @return 指向 `child_view_` 缓存的引用；绘制/布局顺序与 z 序一致。
    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        child_view_.clear();
        if (content_) {
            child_view_.push_back(content_);
        }
        if (panel_) {
            child_view_.push_back(panel_);
        }
        return child_view_;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override;

    auto on_mount(const BuildContext &ctx) -> void override {
        if (content_) {
            content_.widget().mount(ctx);
        }
        if (panel_) {
            panel_.widget().mount(ctx);
        }
    }

    /// @brief 递归卸载内容与面板（与 `on_mount` 逐字对称）。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        if (content_) {
            content_.widget().unmount();
        }
        if (panel_) {
            panel_.widget().unmount();
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (content_) {
            content_.widget().tick(now);
        }
        if (panel_) {
            panel_.widget().tick(now);
        }
    }

  private:
    Node content_;
    Node panel_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> child_view_;
    DrawerSide side_ = DrawerSide::Left;
    float panel_width_ = 240.0F;
    bool permanent_ = false;
    State<bool> open_{false};
    std::function<void(bool)> on_toggle_;
};

/// @brief 进度对话框：模态进度指示 + 消息 + 可选取消按钮。
///
/// `set_progress(0..1)` 更新进度（-1 = 不确定态转圈）；`cancel()` 触发取消回调。
/// 对标 Qt `QProgressDialog`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class ProgressDialog : public Widget {
  public:
    /// @brief 默认构造：空消息、可取消、未显示、进度为不确定态（-1）。
    ProgressDialog() = default;
    /// @brief 以消息文本与可取消性构造（初始为未显示态，需 `show()` 打开）。
    /// @param message 对话框正文（UTF-8），绘制于盒体顶部。
    /// @param cancellable 是否显示并响应取消按钮，默认 true。
    explicit ProgressDialog(std::string message, bool cancellable = true)
        : message_(std::move(message)), cancellable_(cancellable) {}

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"ProgressDialog"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "ProgressDialog"; }

    /// @brief 运行时自描述：message/progress/open/cancellable 属性键、`on_cancel` 事件与取值不变量。
    /// @return ProgressDialog 的控件描述符（实现见 `src/aurora/widget/drawer.cpp`）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    /// @brief 运行时多态自描述入口。
    /// @return 转发 `describe_static()` 的结果。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集响应式信号：仅进度值 `progress_`。
    /// @param out 输出向量：挂载时由基类登记依赖。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&progress_); }

    /// @brief 打开对话框：置显示态并标脏重绘（布局届时才算出盒体与按钮矩形）。
    auto show() -> void {
        open_ = true;
        mark_needs_paint();
    }
    /// @brief 关闭对话框：清显示态并标脏重绘（不触发取消回调）。
    auto close() -> void {
        open_ = false;
        mark_needs_paint();
    }
    /// @brief 当前是否显示。
    /// @return 显示标志（模态遮罩是否覆盖宿主区域）。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 更新进度（0..1；-1 = 不确定态）。
    /// @param v 进度比例；负值一律归一为 -1（不确定态，绘制中段高亮块），其余夹到 [0,1]。
    auto set_progress(float v) -> void {
        progress_.set(v < 0.0F ? -1.0F : std::clamp(v, 0.0F, 1.0F));
        mark_needs_paint();
    }
    /// @brief 当前进度值。
    /// @return 归一后的进度：-1 表示不确定态，否则落在 [0,1]。
    [[nodiscard]] auto progress() const -> float { return progress_.get(); }

    /// @brief 替换消息正文并标脏重绘。
    /// @param msg 新的 UTF-8 文本（移入内部存储）。
    auto set_message(std::string msg) -> void {
        message_ = std::move(msg);
        mark_needs_paint();
    }
    /// @brief 当前消息正文。
    /// @return 指向内部文本的引用（随 `set_message`/反序列化失效，勿长期持有）。
    [[nodiscard]] auto message() const -> const std::string & { return message_; }

    /// @brief 触发取消（可取消时回调 + 关闭）。
    auto cancel() -> void {
        if (!cancellable_) {
            return;
        }
        close();
        if (on_cancel_) {
            on_cancel_();
        }
    }

    /// @brief 设置取消回调（链式）。
    /// @param cb 无参回调，可空；仅在 `cancellable_` 为 true 的取消路径上被调用。
    /// @return 自身引用，便于链式调用。
    auto set_on_cancel(std::function<void()> cb) -> ProgressDialog & {
        on_cancel_ = std::move(cb);
        return *this;
    }

    /// @brief 打开时点击取消按钮区域触发取消。
    /// @param e 鼠标事件；打开态下 Press 命中按钮矩形即 `cancel()`，其余点击作为模态被吞掉。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (open_ && cancellable_ && e.action == MouseAction::Press) {
            if (cancel_rect_.contains(e.local_position)) {
                cancel();
                e.is_handled = true;
                return;
            }
            e.is_handled = true;  // 模态：吞掉其他点击
            return;
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 是否声明点击意图。
    /// @return 显示态为 true（模态需拦截宿主点击）。
    [[nodiscard]] auto wants_click() const -> bool override { return open_; }

    /// @brief 序列化自有属性：message、progress、open、cancellable。
    /// @param props 输出对象，先由基类填通用属性。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("message", message_);
        props.set("progress", progress_.get());
        props.set("open", Json{open_});
        props.set("cancellable", Json{cancellable_});
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override;

  private:
    std::string message_;
    bool cancellable_ = true;
    bool open_ = false;
    State<float> progress_{-1.0F};
    Rect box_rect_;
    Rect cancel_rect_;
    std::function<void()> on_cancel_;
};

/// @brief 页面视图：可翻页容器 + 指示器圆点。
///
/// `current()` 为响应式页码；`next()`/`prev()`/`go_to(i)` 切页；
/// 水平滑动手势翻页（拖拽超过 1/4 宽度）。
///
/// 对标 Flutter `PageView`、SwiftUI `TabView(.page)`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
/// @note 本类隐式生成的拷贝/移动构造逐成员复制 std::function 回调 on_page_change_，而其拷贝与 operator()
///       皆无 noexcept 规格 —— 即 .clang-tidy 记录在案的系统性假告警面。该隐式特成员按 [except.spec]
///       本就是 potentially-throwing，抛出（bad_alloc 或宿主回调自身异常）沿栈交给复制方，本库回调路径
///       刻意不做异常捕获（CODING_STANDARDS.md §2 生命周期回调条目）。
/// NOLINTNEXTLINE(bugprone-exception-escape)
class PageView : public Container {
  public:
    /// @brief 默认构造：无页、页码 0、显示指示器。
    PageView() = default;
    /// @brief 以页面列表构造：接管子节点并夹取初始页码。
    /// @param pages 页面子节点列表（每个 Node 一页），移入 `children_`。
    /// @param initial 期望初始页码；越界时夹到 `[0, size-1]`，空列表时取 0。
    explicit PageView(std::vector<Node> pages, int initial = 0) {
        children_ = std::move(pages);
        const int max_idx = static_cast<int>(children_.size()) - 1;
        current_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    /// @brief widget 类型名（结构快照 JSON 用）。
    /// @return 字面量 `"PageView"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "PageView"; }

    /// @brief 运行时自描述：current/show_indicator 属性键、`on_page_change` 事件、多子节点策略。
    /// @return PageView 的控件描述符（实现见 `src/aurora/widget/drawer.cpp`）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    /// @brief 运行时多态自描述入口。
    /// @return 转发 `describe_static()` 的结果。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集响应式信号：仅当前页码 `current_`。
    /// @param out 输出向量：挂载时由基类登记依赖。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&current_); }

    /// @brief 页数。
    /// @return 子节点数量（即页数，0 表示空容器）。
    [[nodiscard]] auto page_count() const -> std::size_t { return children_.size(); }
    /// @brief 取页码信号本体，供外部双向绑定/订阅。
    /// @return 内部 `State<int>` 引用（0 基页码，不变量 `current >= 0`）。
    [[nodiscard]] auto current() -> State<int> & { return current_; }
    /// @brief 当前页码快照。
    /// @return `current_` 的当前值。
    [[nodiscard]] auto current_page() const -> int { return current_.get(); }

    /// @brief 跳到指定页：越界或同页则整体忽略（不标脏、不回调）。
    /// @param index 目标页码（0 基）；须满足 `0 <= index < page_count()`。
    auto go_to(int index) -> void {
        if (index >= 0 && std::cmp_less(index, children_.size()) && index != current_.get()) {
            current_.set(index);
            mark_needs_layout();
            mark_needs_paint();
            if (on_page_change_) {
                on_page_change_(index);
            }
        }
    }
    /// @brief 翻到下一页（末页时越界被忽略）。
    auto next() -> void { go_to(current_.get() + 1); }
    /// @brief 翻到上一页（首页时负索引被忽略）。
    auto prev() -> void { go_to(current_.get() - 1); }

    /// @brief 设置是否绘制底部指示器圆点（链式）。
    /// @param v true 且页数 > 1 时绘制圆点。
    /// @return 自身引用，便于链式调用。
    auto set_show_indicator(bool v) -> PageView & {
        show_indicator_ = v;
        return *this;
    }

    /// @brief 设置切页回调（链式）。
    /// @param cb 以新页码为参，可空；仅 `go_to` 真正改变页码时调用。
    /// @return 自身引用，便于链式调用。
    auto set_on_page_change(std::function<void(int)> cb) -> PageView & {
        on_page_change_ = std::move(cb);
        return *this;
    }

    /// @brief 水平拖拽翻页：Press 记录起点，Release 时超过 1/4 宽度切页。
    /// @param e 鼠标事件；左滑过阈值进下一页、右滑进上一页，未达阈值的 Release 下沉基类。
    auto on_pointer_event(MouseEvent &e) -> void override {
        switch (e.action) {
            case MouseAction::Press:
                drag_start_x_ = e.local_position.x;
                drag_active_ = true;
                e.is_handled = true;
                return;
            case MouseAction::Release:
                if (drag_active_) {
                    const float dx = e.local_position.x - drag_start_x_;
                    const float threshold = size_.width * 0.25F;
                    if (dx <= -threshold) {
                        next();  // 左滑下一页
                    } else if (dx >= threshold) {
                        prev();  // 右滑上一页
                    }
                    drag_active_ = false;
                    e.is_handled = true;
                    return;
                }
                break;
            default:
                break;
        }
        Widget::on_pointer_event(e);
    }

    /// @brief 是否声明点击意图。
    /// @return 恒 true：拖拽翻页需始终拿到 Press/Release 序列。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    /// @brief 序列化自有属性：current 页码与 show_indicator 开关。
    /// @param props 输出对象，先由基类填通用属性（子节点另由容器序列化）。
    /// @note Rebuildable: yes, via from_json
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("current", current_.get());
        props.set("show_indicator", Json{show_indicator_});
    }

    /// @brief 从 props JSON 还原自有属性（缺失键保持当前值）。
    /// @param props 属性 JSON；`current` 直接写入信号，不做越界夹取（越界页由布局阶段忽略）。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("current")) {
            current_.set(props.at("current")->as_or<std::int32_t>(0));
        }
        if (props.contains("show_indicator")) {
            show_indicator_ = props.at("show_indicator")->as_or<bool>(false);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override;

  private:
    State<int> current_{0};
    bool show_indicator_ = true;
    bool drag_active_ = false;
    float drag_start_x_ = 0.0F;
    std::function<void(int)> on_page_change_;
};

}  // namespace aurora
