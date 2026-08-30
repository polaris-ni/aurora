#pragma once

/// @file modifier_input.h
/// @brief 输入修饰节点（Input 切片）：Clickable / Draggable / LongPress / TouchListener /
/// TooltipNode / ContextMenuNode。
/// 本文件为 modifier.h 的子切片；消费者通常直接 #include "aurora/modifier/modifier.h"。

#include "aurora/app/menu.h"
#include "aurora/modifier/modifier_base.h"

namespace aurora {

/// @brief 可点击修饰：不影响尺寸，命中时拦截事件（执行 onTap）。
class Clickable : public ModifierNode {
  public:
    explicit Clickable(std::function<void()> on_tap) : m_on_tap(std::move(on_tap)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    auto on_tap() const -> void {
        if (m_on_tap) {
            m_on_tap();
        }
    }

    auto fire_click() const -> void override { on_tap(); }

  private:
    std::function<void()> m_on_tap;
};

/// @brief 可拖拽修饰（Input 切片）：按下并移动时回调上报位移增量与绝对坐标（不改布局）。
class Draggable : public ModifierNode {
  public:
    using DragCallback = std::function<void(Point delta, Point pos)>;

    explicit Draggable(DragCallback on_drag, std::function<void()> on_start = {}, std::function<void()> on_end = {})
        : m_on_drag(std::move(on_drag)), m_on_start(std::move(on_start)), m_on_end(std::move(on_end)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的拖拽被处理。
    ///        鼠标事件（pointer_id 为 nullopt）视为「任意指针」，始终绑定。
    auto bind(std::optional<int> pid) const -> void {
        if (!m_pointer_id.has_value()) {
            m_pointer_id = pid;
        }
    }
    /// @brief 该指针是否属于本拖拽（未绑定或同 id）。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !m_pointer_id.has_value() || m_pointer_id == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { m_pointer_id.reset(); }

    auto fire_start() const -> void {
        if (m_on_start) {
            m_on_start();
        }
    }
    auto fire_drag(const Point &delta, const Point &pos) const -> void {
        if (m_on_drag) {
            m_on_drag(delta, pos);
        }
    }
    auto fire_end() const -> void {
        if (m_on_end) {
            m_on_end();
        }
    }

  private:
    DragCallback m_on_drag;
    std::function<void()> m_on_start;
    std::function<void()> m_on_end;
    mutable std::optional<int> m_pointer_id;
};

/// @brief 长按修饰（Input 切片）：按下并保持超过阈值（默认 500ms）后触发回调。
/// 需要渲染/事件循环周期性调用 `Widget::tickGestures` 触发（详见 application.h::tick）。
class LongPress : public ModifierNode {
  public:
    explicit LongPress(std::function<void()> on_long_press, float threshold_ms = 500.0f)
        : m_on_long_press(std::move(on_long_press)), m_threshold(threshold_ms) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的长按计时生效。
    auto bind(std::optional<int> pid) const -> void {
        if (!m_pointer_id.has_value()) {
            m_pointer_id = pid;
        }
    }
    /// @brief 该指针是否属于本长按（未绑定或同 id）。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !m_pointer_id.has_value() || m_pointer_id == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { m_pointer_id.reset(); }

    auto press_at(std::chrono::steady_clock::time_point t) -> void {
        m_start = t;
        m_fired = false;
    }

    auto cancel() -> void { m_start.reset(); }

    /// @brief 是否已触发过长按回调（用于点击/长按互斥：已触发则抑制点击）。
    [[nodiscard]] auto long_press_fired() const -> bool { return m_fired; }

    /// @brief 检查是否到达长按阈值；到达则触发一次回调（幂等）。
    auto tick(std::chrono::steady_clock::time_point now) -> void {
        if (m_fired || !m_start.has_value()) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start.value()).count();
        if (static_cast<float>(ms) >= m_threshold) {
            m_fired = true;
            if (m_on_long_press) {
                m_on_long_press();
            }
        }
    }

  private:
    std::function<void()> m_on_long_press;
    float m_threshold = 0.0f;
    std::optional<std::chrono::steady_clock::time_point> m_start;
    mutable std::optional<int> m_pointer_id;
    bool m_fired = false;
};

/// @brief 原始多点触摸监听修饰（Input 切片）：每次 `TouchEvent` 派发到该 widget 时回调完整事件，
///        供上层自定义并发交互（如多指手势、自定义转场），不消费命中、不影响布局。
class TouchListener : public ModifierNode {
  public:
    explicit TouchListener(std::function<void(const TouchEvent &)> on_touch) : m_on_touch(std::move(on_touch)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    auto on_touch(const TouchEvent &e) const -> void override {
        if (m_on_touch) {
            m_on_touch(e);
        }
    }

  private:
    std::function<void(const TouchEvent &)> m_on_touch;
};

/// @brief 工具提示修饰（Input 切片）：鼠标悬停延迟后显示提示气泡。
/// 对标 Qt `QToolTip`、WPF `ToolTip`、Flutter `Tooltip`、SwiftUI `.help()`。
class TooltipNode : public ModifierNode {
  public:
    explicit TooltipNode(std::string text, float delay_ms = 500.0f)
        : m_text(std::move(text)), m_delay_ms(delay_ms < 0.0f ? 0.0f : delay_ms) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto text() const -> const std::string & { return m_text; }
    [[nodiscard]] auto delay_ms() const -> float { return m_delay_ms; }

    /// @brief 鼠标进入时开始计时。
    auto hover_start(std::chrono::steady_clock::time_point t) const -> void {
        m_hover_start = t;
        m_visible = false;
    }

    /// @brief 鼠标离开时重置。
    auto hover_end() const -> void {
        m_hover_start.reset();
        m_visible = false;
    }

    /// @brief 周期性检查是否到达延迟阈值；到达则标记可见（幂等）。
    auto tick(std::chrono::steady_clock::time_point now) const -> void {
        if (m_visible || !m_hover_start.has_value()) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_hover_start.value()).count();
        if (static_cast<float>(ms) >= m_delay_ms) {
            m_visible = true;
        }
    }

    /// @brief 当前是否应显示提示。
    [[nodiscard]] auto is_visible() const -> bool { return m_visible; }

  private:
    std::string m_text;
    float m_delay_ms = 0.0f;
    mutable std::optional<std::chrono::steady_clock::time_point> m_hover_start;
    mutable bool m_visible = false;
};

/// @brief 上下文菜单修饰（Input 切片）：右键点击时弹出浮动菜单。
/// 对标 Qt `QMenu::exec()`、SwiftUI `.contextMenu{}`、WPF `ContextMenu`。
class ContextMenuNode : public ModifierNode {
  public:
    explicit ContextMenuNode(std::vector<MenuItem> items) : m_items(std::move(items)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto items() const -> const std::vector<MenuItem> & { return m_items; }

    /// @brief 右键按下时触发：标记菜单打开并记录弹出位置。
    auto open_at(Point pos) const -> void {
        m_open = true;
        m_position = pos;
    }

    /// @brief 关闭菜单。
    auto close() const -> void { m_open = false; }

    /// @brief 当前是否打开。
    [[nodiscard]] auto is_open() const -> bool { return m_open; }

    /// @brief 弹出位置（全局坐标）。
    [[nodiscard]] auto position() const -> Point { return m_position; }

  private:
    std::vector<MenuItem> m_items;
    mutable bool m_open = false;
    mutable Point m_position{ .x = 0.0f, .y = 0.0f };
};

} // namespace aurora
