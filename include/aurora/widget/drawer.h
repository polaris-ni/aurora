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
    Drawer(Node content, Node panel, DrawerSide side = DrawerSide::Left, float panel_width = 240.0f)
        : m_content(std::move(content)), m_panel(std::move(panel)), m_side(side),
          m_panel_width(panel_width > 0.0f ? panel_width : 240.0f) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Drawer"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_open); }

    [[nodiscard]] auto is_open() const -> bool { return m_open.get(); }
    [[nodiscard]] auto open_state() -> State<bool> & { return m_open; }
    [[nodiscard]] auto is_permanent() const -> bool { return m_permanent; }
    [[nodiscard]] auto panel_width() const -> float { return m_panel_width; }

    auto set_open(bool v) -> void {
        if (m_permanent) {
            return; // 永久模式无开合
        }
        if (v != m_open.get()) {
            m_open.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (m_on_toggle) {
                m_on_toggle(v);
            }
        }
    }
    auto toggle() -> void { set_open(!m_open.get()); }

    /// @brief 永久模式（链式）：面板始终可见、无遮罩、内容让位。
    auto set_permanent(bool v) -> Drawer & {
        m_permanent = v;
        mark_needs_layout();
        return *this;
    }

    auto set_on_toggle(std::function<void(bool)> cb) -> Drawer & {
        m_on_toggle = std::move(cb);
        return *this;
    }

    /// @brief 模态打开时点击遮罩（面板外）关闭。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (!m_permanent && m_open.get() && e.action == MouseAction::Press) {
            const Rect panel = panel_rect();
            if (!panel.contains(e.local_position)) {
                set_open(false);
                e.handled = true;
                return;
            }
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override {
        return !m_permanent && m_open.get(); // 打开时拦截遮罩点击
    }

    /// @brief 面板矩形（本控件局部坐标）。
    [[nodiscard]] auto panel_rect() const -> Rect {
        const float w = std::min(m_panel_width, m_size.width);
        return m_side == DrawerSide::Left ? Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                                  .size = Size{ .width = w, .height = m_size.height } }
                                          : Rect{ .origin = Point{ .x = m_size.width - w, .y = 0.0f },
                                                  .size = Size{ .width = w, .height = m_size.height } };
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["open"] = m_open.get();
        props["side"] = m_side == DrawerSide::Left ? "left" : "right";
        props["panel_width"] = m_panel_width;
        props["permanent"] = m_permanent;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            m_open.set(props["open"].get<bool>());
        }
        if (props.contains("side")) {
            m_side = props["side"].get<std::string>() == "right" ? DrawerSide::Right : DrawerSide::Left;
        }
        if (props.contains("panel_width")) {
            m_panel_width = props["panel_width"].get<float>();
        }
        if (props.contains("permanent")) {
            m_permanent = props["permanent"].get<bool>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (m_content) {
            fn(m_content.widget());
        }
        if (m_panel) {
            fn(m_panel.widget());
        }
    }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        m_child_view.clear();
        if (m_content) {
            m_child_view.push_back(m_content);
        }
        if (m_panel) {
            m_child_view.push_back(m_panel);
        }
        return m_child_view;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override;

    auto on_mount(const BuildContext &ctx) -> void override {
        if (m_content) {
            m_content.widget().mount(ctx);
        }
        if (m_panel) {
            m_panel.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (m_content) {
            m_content.widget().tick(now);
        }
        if (m_panel) {
            m_panel.widget().tick(now);
        }
    }

  private:
    Node m_content;
    Node m_panel;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> m_child_view;
    DrawerSide m_side = DrawerSide::Left;
    float m_panel_width = 240.0f;
    bool m_permanent = false;
    State<bool> m_open{ false };
    std::function<void(bool)> m_on_toggle;
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
        : m_message(std::move(message)), m_cancellable(cancellable) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "ProgressDialog"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_progress); }

    auto show() -> void {
        m_open = true;
        mark_needs_paint();
    }
    auto close() -> void {
        m_open = false;
        mark_needs_paint();
    }
    [[nodiscard]] auto is_open() const -> bool { return m_open; }

    /// @brief 更新进度（0..1；-1 = 不确定态）。
    auto set_progress(float v) -> void {
        m_progress.set(v < 0.0f ? -1.0f : std::clamp(v, 0.0f, 1.0f));
        mark_needs_paint();
    }
    [[nodiscard]] auto progress() const -> float { return m_progress.get(); }

    auto set_message(std::string msg) -> void {
        m_message = std::move(msg);
        mark_needs_paint();
    }
    [[nodiscard]] auto message() const -> const std::string & { return m_message; }

    /// @brief 触发取消（可取消时回调 + 关闭）。
    auto cancel() -> void {
        if (!m_cancellable) {
            return;
        }
        close();
        if (m_on_cancel) {
            m_on_cancel();
        }
    }

    auto set_on_cancel(std::function<void()> cb) -> ProgressDialog & {
        m_on_cancel = std::move(cb);
        return *this;
    }

    /// @brief 打开时点击取消按钮区域触发取消。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (m_open && m_cancellable && e.action == MouseAction::Press) {
            if (m_cancel_rect.contains(e.local_position)) {
                cancel();
                e.handled = true;
                return;
            }
            e.handled = true; // 模态：吞掉其他点击
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return m_open; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["message"] = m_message;
        props["progress"] = m_progress.get();
        props["open"] = m_open;
        props["cancellable"] = m_cancellable;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext & /*ctx*/) -> Widget * override;

  private:
    std::string m_message;
    bool m_cancellable = true;
    bool m_open = false;
    State<float> m_progress{ -1.0f };
    Rect m_box_rect;
    Rect m_cancel_rect;
    std::function<void()> m_on_cancel;
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
        m_children = std::move(pages);
        const int max_idx = static_cast<int>(m_children.size()) - 1;
        m_current.set(std::clamp(initial, 0, std::max(0, max_idx)));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "PageView"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;

    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_current); }

    [[nodiscard]] auto page_count() const -> std::size_t { return m_children.size(); }
    [[nodiscard]] auto current() -> State<int> & { return m_current; }
    [[nodiscard]] auto current_page() const -> int { return m_current.get(); }

    auto go_to(int index) -> void {
        if (index >= 0 && std::cmp_less(index, m_children.size()) && index != m_current.get()) {
            m_current.set(index);
            mark_needs_layout();
            mark_needs_paint();
            if (m_on_page_change) {
                m_on_page_change(index);
            }
        }
    }
    auto next() -> void { go_to(m_current.get() + 1); }
    auto prev() -> void { go_to(m_current.get() - 1); }

    auto set_show_indicator(bool v) -> PageView & {
        m_show_indicator = v;
        return *this;
    }

    auto set_on_page_change(std::function<void(int)> cb) -> PageView & {
        m_on_page_change = std::move(cb);
        return *this;
    }

    /// @brief 水平拖拽翻页：Press 记录起点，Release 时超过 1/4 宽度切页。
    auto on_pointer_event(MouseEvent &e) -> void override {
        switch (e.action) {
        case MouseAction::Press:
            m_drag_start_x = e.local_position.x;
            m_drag_active = true;
            e.handled = true;
            return;
        case MouseAction::Release:
            if (m_drag_active) {
                const float dx = e.local_position.x - m_drag_start_x;
                const float threshold = m_size.width * 0.25f;
                if (dx <= -threshold) {
                    next(); // 左滑下一页
                } else if (dx >= threshold) {
                    prev(); // 右滑上一页
                }
                m_drag_active = false;
                e.handled = true;
                return;
            }
            break;
        default: break;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["current"] = m_current.get();
        props["show_indicator"] = m_show_indicator;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("current")) {
            m_current.set(props["current"].get<int>());
        }
        if (props.contains("show_indicator")) {
            m_show_indicator = props["show_indicator"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override;

  private:
    State<int> m_current{ 0 };
    bool m_show_indicator = true;
    bool m_drag_active = false;
    float m_drag_start_x = 0.0f;
    std::function<void(int)> m_on_page_change;
};

} // namespace aurora
