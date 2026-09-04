#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 抽屉停靠侧。
enum class DrawerSide : std::uint8_t { Left, Right };

/**
 * @brief 抽屉/侧边栏：可滑出的侧边面板。
 *
 * 两子结构：基础内容（占满）+ 抽屉面板（左/右停靠）。打开时绘制半透明遮罩
 * （模态语义），点击遮罩关闭；`permanent` 模式下面板始终可见、无遮罩、
 * 基础内容让出面板宽度。
 *
 * 对标 Flutter `Drawer`/`EndDrawer`、Qt `QDockWidget`、SwiftUI `NavigationSplitView`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Drawer : public Widget {
  public:
    Drawer() = default;
    Drawer(Node content, Node panel, DrawerSide side = DrawerSide::Left, float panel_width = 240.0F)
        : content_(std::move(content)), panel_(std::move(panel)), side_(side),
          panel_width_(panel_width > 0.0F ? panel_width : 240.0F) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Drawer"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&open_); }

    [[nodiscard]] auto is_open() const -> bool { return open_.get(); }
    [[nodiscard]] auto open_state() -> State<bool> & { return open_; }
    [[nodiscard]] auto is_permanent() const -> bool { return permanent_; }
    [[nodiscard]] auto panel_width() const -> float { return panel_width_; }

    auto set_open(bool v) -> void {
        if (permanent_) {
            return;  // 永久模式无开合
        }
        if (v != open_.get()) {
            open_.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (on_toggle_) {
                on_toggle_(v);
            }
        }
    }
    auto toggle() -> void { set_open(!open_.get()); }

    /// @brief 永久模式（链式）：面板始终可见、无遮罩、内容让位。
    auto set_permanent(bool v) -> Drawer & {
        permanent_ = v;
        mark_needs_layout();
        return *this;
    }

    auto set_on_toggle(std::function<void(bool)> cb) -> Drawer & {
        on_toggle_ = std::move(cb);
        return *this;
    }

    /// @brief 模态打开时点击遮罩（面板外）关闭。
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

    [[nodiscard]] auto wants_click() const -> bool override {
        return !permanent_ && open_.get();  // 打开时拦截遮罩点击
    }

    /// @brief 面板矩形（本控件局部坐标）。
    [[nodiscard]] auto panel_rect() const -> Rect {
        const float w = std::min(panel_width_, size_.width);
        return side_ == DrawerSide::Left
                   ? Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = size_.height}}
                   : Rect{.origin = Point{.x = size_.width - w, .y = 0.0F},
                          .size = Size{.width = w, .height = size_.height}};
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["open"] = open_.get();
        props["side"] = side_ == DrawerSide::Left ? "left" : "right";
        props["panel_width"] = panel_width_;
        props["permanent"] = permanent_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            open_.set(props["open"].get<bool>());
        }
        if (props.contains("side")) {
            side_ = props["side"].get<std::string>() == "right" ? DrawerSide::Right : DrawerSide::Left;
        }
        if (props.contains("panel_width")) {
            panel_width_ = props["panel_width"].get<float>();
        }
        if (props.contains("permanent")) {
            permanent_ = props["permanent"].get<bool>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (content_) {
            fn(content_.widget());
        }
        if (panel_) {
            fn(panel_.widget());
        }
    }

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

/**
 * @brief 进度对话框：模态进度指示 + 消息 + 可选取消按钮。
 *
 * `set_progress(0..1)` 更新进度（-1 = 不确定态转圈）；`cancel()` 触发取消回调。
 * 对标 Qt `QProgressDialog`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ProgressDialog : public Widget {
  public:
    ProgressDialog() = default;
    explicit ProgressDialog(std::string message, bool cancellable = true)
        : message_(std::move(message)), cancellable_(cancellable) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ProgressDialog"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&progress_); }

    auto show() -> void {
        open_ = true;
        mark_needs_paint();
    }
    auto close() -> void {
        open_ = false;
        mark_needs_paint();
    }
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 更新进度（0..1；-1 = 不确定态）。
    auto set_progress(float v) -> void {
        progress_.set(v < 0.0F ? -1.0F : std::clamp(v, 0.0F, 1.0F));
        mark_needs_paint();
    }
    [[nodiscard]] auto progress() const -> float { return progress_.get(); }

    auto set_message(std::string msg) -> void {
        message_ = std::move(msg);
        mark_needs_paint();
    }
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

    auto set_on_cancel(std::function<void()> cb) -> ProgressDialog & {
        on_cancel_ = std::move(cb);
        return *this;
    }

    /// @brief 打开时点击取消按钮区域触发取消。
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

    [[nodiscard]] auto wants_click() const -> bool override { return open_; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["message"] = message_;
        props["progress"] = progress_.get();
        props["open"] = open_;
        props["cancellable"] = cancellable_;
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

/**
 * @brief 页面视图：可翻页容器 + 指示器圆点。
 *
 * `current()` 为响应式页码；`next()`/`prev()`/`go_to(i)` 切页；
 * 水平滑动手势翻页（拖拽超过 1/4 宽度）。
 *
 * 对标 Flutter `PageView`、SwiftUI `TabView(.page)`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class PageView : public Container {
  public:
    PageView() = default;
    explicit PageView(std::vector<Node> pages, int initial = 0) {
        children_ = std::move(pages);
        const int max_idx = static_cast<int>(children_.size()) - 1;
        current_.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "PageView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&current_); }

    [[nodiscard]] auto page_count() const -> std::size_t { return children_.size(); }
    [[nodiscard]] auto current() -> State<int> & { return current_; }
    [[nodiscard]] auto current_page() const -> int { return current_.get(); }

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
    auto next() -> void { go_to(current_.get() + 1); }
    auto prev() -> void { go_to(current_.get() - 1); }

    auto set_show_indicator(bool v) -> PageView & {
        show_indicator_ = v;
        return *this;
    }

    auto set_on_page_change(std::function<void(int)> cb) -> PageView & {
        on_page_change_ = std::move(cb);
        return *this;
    }

    /// @brief 水平拖拽翻页：Press 记录起点，Release 时超过 1/4 宽度切页。
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

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["current"] = current_.get();
        props["show_indicator"] = show_indicator_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("current")) {
            current_.set(props["current"].get<int>());
        }
        if (props.contains("show_indicator")) {
            show_indicator_ = props["show_indicator"].get<bool>();
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
