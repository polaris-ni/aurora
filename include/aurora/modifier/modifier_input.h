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
    explicit Clickable(std::function<void()> on_tap) : on_tap_(std::move(on_tap)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    auto on_tap() const -> void {
        if (on_tap_) {
            on_tap_();
        }
    }

    auto fire_click() const -> void override { on_tap(); }

  private:
    std::function<void()> on_tap_;
};

/// @brief 可拖拽修饰（Input 切片）：按下并移动时回调上报位移增量与绝对坐标（不改布局）。
class Draggable : public ModifierNode {
  public:
    using DragCallback = std::function<void(Point delta, Point pos)>;

    explicit Draggable(DragCallback on_drag, std::function<void()> on_start = {}, std::function<void()> on_end = {})
        : on_drag_(std::move(on_drag)), on_start_(std::move(on_start)), on_end_(std::move(on_end)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的拖拽被处理。
    ///        鼠标事件（pointer_id 为 nullopt）视为「任意指针」，始终绑定。
    auto bind(std::optional<int> pid) const -> void {
        if (!pointer_id_.has_value()) {
            pointer_id_ = pid;
        }
    }
    /// @brief 该指针是否属于本拖拽（未绑定或同 id）。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !pointer_id_.has_value() || pointer_id_ == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { pointer_id_.reset(); }

    auto fire_start() const -> void {
        if (on_start_) {
            on_start_();
        }
    }
    auto fire_drag(const Point &delta, const Point &pos) const -> void {
        if (on_drag_) {
            on_drag_(delta, pos);
        }
    }
    auto fire_end() const -> void {
        if (on_end_) {
            on_end_();
        }
    }

  private:
    DragCallback on_drag_;
    std::function<void()> on_start_;
    std::function<void()> on_end_;
    mutable std::optional<int> pointer_id_;
};

/// @brief 长按修饰（Input 切片）：按下并保持超过阈值（默认 500ms）后触发回调。
/// 需要渲染/事件循环周期性调用 `Widget::tickGestures` 触发（详见 application.h::tick）。
class LongPress : public ModifierNode {
  public:
    explicit LongPress(std::function<void()> on_long_press, float threshold_ms = 500.0F)
        : on_long_press_(std::move(on_long_press)), threshold_(threshold_ms) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的长按计时生效。
    auto bind(std::optional<int> pid) const -> void {
        if (!pointer_id_.has_value()) {
            pointer_id_ = pid;
        }
    }
    /// @brief 该指针是否属于本长按（未绑定或同 id）。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !pointer_id_.has_value() || pointer_id_ == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { pointer_id_.reset(); }

    auto press_at(std::chrono::steady_clock::time_point t) -> void {
        start_ = t;
        fired_ = false;
    }

    auto cancel() -> void { start_.reset(); }

    /// @brief 是否已触发过长按回调（用于点击/长按互斥：已触发则抑制点击）。
    [[nodiscard]] auto long_press_fired() const -> bool { return fired_; }

    /// @brief 检查是否到达长按阈值；到达则触发一次回调（幂等）。
    auto tick(std::chrono::steady_clock::time_point now) -> void {
        if (fired_ || !start_.has_value()) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_.value()).count();
        if (static_cast<float>(ms) >= threshold_) {
            fired_ = true;
            if (on_long_press_) {
                on_long_press_();
            }
        }
    }

  private:
    std::function<void()> on_long_press_;
    float threshold_ = 0.0F;
    std::optional<std::chrono::steady_clock::time_point> start_;
    mutable std::optional<int> pointer_id_;
    bool fired_ = false;
};

/// @brief 原始多点触摸监听修饰（Input 切片）：每次 `TouchEvent` 派发到该 widget 时回调完整事件，
///        供上层自定义并发交互（如多指手势、自定义转场），不消费命中、不影响布局。
class TouchListener : public ModifierNode {
  public:
    explicit TouchListener(std::function<void(const TouchEvent &)> on_touch) : on_touch_(std::move(on_touch)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    auto on_touch(const TouchEvent &e) const -> void override {
        if (on_touch_) {
            on_touch_(e);
        }
    }

  private:
    std::function<void(const TouchEvent &)> on_touch_;
};

/// @brief 工具提示修饰（Input 切片）：鼠标悬停延迟后显示提示气泡。
/// 对标 Qt `QToolTip`、WPF `ToolTip`、Flutter `Tooltip`、SwiftUI `.help()`。
class TooltipNode : public ModifierNode {
  public:
    explicit TooltipNode(std::string text, float delay_ms = 500.0F)
        : text_(std::move(text)), delay_ms_(delay_ms < 0.0F ? 0.0F : delay_ms) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto text() const -> const std::string & { return text_; }
    [[nodiscard]] auto delay_ms() const -> float { return delay_ms_; }

    /// @brief 鼠标进入时开始计时。
    auto hover_start(std::chrono::steady_clock::time_point t) const -> void {
        hover_start_ = t;
        visible_ = false;
    }

    /// @brief 鼠标离开时重置。
    auto hover_end() const -> void {
        hover_start_.reset();
        visible_ = false;
    }

    /// @brief 周期性检查是否到达延迟阈值；到达则标记可见（幂等）。
    auto tick(std::chrono::steady_clock::time_point now) const -> void {
        if (visible_ || !hover_start_.has_value()) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - hover_start_.value()).count();
        if (static_cast<float>(ms) >= delay_ms_) {
            visible_ = true;
        }
    }

    /// @brief 当前是否应显示提示。
    [[nodiscard]] auto is_visible() const -> bool { return visible_; }

  private:
    std::string text_;
    float delay_ms_ = 0.0F;
    mutable std::optional<std::chrono::steady_clock::time_point> hover_start_;
    mutable bool visible_ = false;
};

/// @brief 上下文菜单修饰（Input 切片）：右键点击时弹出浮动菜单。
/// 对标 Qt `QMenu::exec()`、SwiftUI `.contextMenu{}`、WPF `ContextMenu`。
class ContextMenuNode : public ModifierNode {
  public:
    explicit ContextMenuNode(std::vector<MenuItem> items) : items_(std::move(items)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto items() const -> const std::vector<MenuItem> & { return items_; }

    /// @brief 右键按下时触发：标记菜单打开并记录弹出位置。
    auto open_at(Point pos) const -> void {
        open_ = true;
        position_ = pos;
    }

    /// @brief 关闭菜单。
    auto close() const -> void { open_ = false; }

    /// @brief 当前是否打开。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 弹出位置（全局坐标）。
    [[nodiscard]] auto position() const -> Point { return position_; }

  private:
    std::vector<MenuItem> items_;
    mutable bool open_ = false;
    mutable Point position_{.x = 0.0F, .y = 0.0F};
};

}  // namespace aurora
