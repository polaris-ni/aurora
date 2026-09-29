#pragma once

/// @brief 输入修饰节点（Input 切片）：Clickable / Draggable / LongPress / TouchListener /
/// TooltipNode / ContextMenuNode / CursorNode。
/// @file modifier_input.h
/// 本文件为 modifier.h 的子切片；消费者通常直接 `#include` "aurora/modifier/modifier.h"。

#include <optional>

#include "aurora/app/menu.h"
#include "aurora/core/enums.h"
#include "aurora/modifier/modifier_base.h"

namespace aurora {

/// @brief 鼠标光标形状修饰：悬停本控件时把光标切到声明形状（不影响尺寸/命中）。
/// 解析优先级（见 `EventDispatcher` 悬停链光标解析）：修饰链上的 `CursorNode`
/// 优先于 `Widget::cursor_shape()` 虚钩子；同链多个 `CursorNode` 取最后一个。
class CursorNode : public ModifierNode {
  public:
    /// @brief 以声明的光标形状构造节点。
    /// @param shape 悬停本控件时要切换到的光标形状。
    explicit CursorNode(CursorShape shape) : shape_(shape) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 悬停链光标解析消费的虚钩子。
    /// @return 恒有值：构造时声明的形状（优先于 `Widget::cursor_shape()`）。
    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return shape_; }

    /// @brief 直读形状。
    /// @return 构造时传入的光标形状。
    [[nodiscard]] auto shape() const -> CursorShape { return shape_; }

  private:
    CursorShape shape_;
};

/// @brief 可点击修饰：不影响尺寸，命中时拦截事件（执行 onTap）。
class Clickable : public ModifierNode {
  public:
    /// @brief 以点击回调构造节点。
    /// @param on_tap 点击回调；可为空（为空时点击静默无动作）。
    explicit Clickable(std::function<void()> on_tap) : on_tap_(std::move(on_tap)) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 执行注册的点击回调。
    auto on_tap() const -> void {
        if (on_tap_) {
            on_tap_();
        }
    }

    /// @brief 基类点击入口：转发到 `on_tap()`。
    auto fire_click() const -> void override { on_tap(); }

  private:
    std::function<void()> on_tap_;
};

/// @brief 可拖拽修饰（Input 切片）：按下并移动时回调上报位移增量与绝对坐标（不改布局）。
class Draggable : public ModifierNode {
  public:
    /// @brief 拖拽回调类型：上报相对上次通知的位移增量与当前绝对坐标。
    using DragCallback = std::function<void(Point delta, Point pos)>;

    /// @brief 以拖拽回调与起止回调构造节点。
    /// @param on_drag 每次位移时执行的拖拽回调（必需）。
    /// @param on_start 按下开始拖拽时的回调；可为空。
    /// @param on_end 抬起结束拖拽时的回调；可为空。
    explicit Draggable(DragCallback on_drag, std::function<void()> on_start = {}, std::function<void()> on_end = {})
        : on_drag_(std::move(on_drag)), on_start_(std::move(on_start)), on_end_(std::move(on_end)) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的拖拽被处理。
    ///        鼠标事件（pointer_id 为 nullopt）视为「任意指针」，始终绑定。
    /// @param pid 本次按下的指针 id；nullopt 表示鼠标。
    auto bind(std::optional<int> pid) const -> void {
        if (!pointer_id_.has_value()) {
            pointer_id_ = pid;
        }
    }
    /// @brief 该指针是否属于本拖拽（未绑定或同 id）。
    /// @param pid 待检查的指针 id。
    /// @return true 表示本拖拽尚未绑定指针，或 `pid` 与已绑定 id 相同。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !pointer_id_.has_value() || pointer_id_ == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { pointer_id_.reset(); }

    /// @brief 执行拖拽开始回调（未注册时为空操作）。
    auto fire_start() const -> void {
        if (on_start_) {
            on_start_();
        }
    }
    /// @brief 执行拖拽位移回调（未注册时为空操作）。
    /// @param delta 相对上次通知的位移增量。
    /// @param pos 当前绝对坐标。
    auto fire_drag(const Point &delta, const Point &pos) const -> void {
        if (on_drag_) {
            on_drag_(delta, pos);
        }
    }
    /// @brief 执行拖拽结束回调（未注册时为空操作）。
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
    /// @brief 以长按回调与阈值构造节点。
    /// @param on_long_press 到达阈值时执行的回调；可为空（为空仅置已触发标记）。
    /// @param threshold_ms 长按判定阈值（毫秒），默认 500。
    explicit LongPress(std::function<void()> on_long_press, float threshold_ms = 500.0F)
        : on_long_press_(std::move(on_long_press)), threshold_(threshold_ms) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 按下时绑定指针：未绑定则记录 pointer id，使后续仅同指针的长按计时生效。
    /// @param pid 本次按下的指针 id；nullopt 表示鼠标。
    auto bind(std::optional<int> pid) const -> void {
        if (!pointer_id_.has_value()) {
            pointer_id_ = pid;
        }
    }
    /// @brief 该指针是否属于本长按（未绑定或同 id）。
    /// @param pid 待检查的指针 id。
    /// @return true 表示本长按尚未绑定指针，或 `pid` 与已绑定 id 相同。
    [[nodiscard]] auto matches(std::optional<int> pid) const -> bool {
        return !pointer_id_.has_value() || pointer_id_ == pid;
    }
    /// @brief 抬起后解绑，允许下一次按下重新绑定。
    auto release() const -> void { pointer_id_.reset(); }

    /// @brief 按下时记录起点并复位触发标记，开始一次长按保持。
    /// @param t 按下时刻（steady_clock 时间）。
    auto press_at(std::chrono::steady_clock::time_point t) -> void {
        start_ = t;
        fired_ = false;
    }

    /// @brief 取消进行中的长按保持：清除按下起点，此后 `tick` 不再触发。
    auto cancel() -> void { start_.reset(); }

    /// @brief 是否已触发过长按回调（用于点击/长按互斥：已触发则抑制点击）。
    /// @return true 表示本次保持中已到达阈值并触发过回调（`press_at` 会复位该标记）。
    [[nodiscard]] auto long_press_fired() const -> bool { return fired_; }

    /// @brief 检查是否到达长按阈值；到达则触发一次回调（幂等）。
    /// @param now 当前时刻（steady_clock 时间），与按下起点比较。
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
///        供上层自定义并发交互（如多指手势、自定义转场）。当前实现中 Input 类节点一经命中即返回自身，
///        会拦截向子节点下探（含 TouchListener / Tooltip / Cursor）；不影响布局。
class TouchListener : public ModifierNode {
  public:
    /// @brief 以触摸回调构造节点。
    /// @param on_touch 收到原始触摸事件时的回调；可为空。
    explicit TouchListener(std::function<void(const TouchEvent &)> on_touch) : on_touch_(std::move(on_touch)) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 覆写基类钩子：把派发到本 widget 的触摸事件原样转发给上层回调。
    /// @param e 完整的原始触摸事件。
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
    /// @brief 以提示文本与悬停延迟构造节点。
    /// @param text 提示气泡显示的文本。
    /// @param delay_ms 悬停到弹出的延迟（毫秒）；负值在构造时钳为 0，默认 500。
    explicit TooltipNode(std::string text, float delay_ms = 500.0F)
        : text_(std::move(text)), delay_ms_(delay_ms < 0.0F ? 0.0F : delay_ms) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取提示文本。
    /// @return 节点持有的文本引用。
    [[nodiscard]] auto text() const -> const std::string & { return text_; }
    /// @brief 读取悬停延迟。
    /// @return 延迟毫秒数（构造时已钳为非负）。
    [[nodiscard]] auto delay_ms() const -> float { return delay_ms_; }

    /// @brief 鼠标进入时开始计时。
    /// @param t 进入时刻（steady_clock 时间），同时清除当前可见标记。
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
    /// @param now 当前时刻（steady_clock 时间），与进入起点比较。
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
    /// @return true 表示悬停已到达延迟阈值且未离开。
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
    /// @brief 以菜单项列表构造节点。
    /// @param items 右键弹出时显示的菜单项，构造时移入节点。
    explicit ContextMenuNode(std::vector<MenuItem> items) : items_(std::move(items)) {}

    /// @brief 声明节点类别。
    /// @return Kind::Input（输入修饰切片）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Input; }

    /// @brief 布局直传：不改约束、不加尺寸，仅度量子节点。
    /// @param c 入向约束，原样转发给子节点。
    /// @param measure_child 子节点度量回调。
    /// @return 子节点的度量结果。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取菜单项列表。
    /// @return 节点持有的菜单项引用。
    [[nodiscard]] auto items() const -> const std::vector<MenuItem> & { return items_; }

    /// @brief 右键按下时触发：标记菜单打开并记录弹出位置。
    /// @param pos 弹出锚点（全局坐标）。
    auto open_at(Point pos) const -> void {
        open_ = true;
        position_ = pos;
    }

    /// @brief 关闭菜单。
    auto close() const -> void { open_ = false; }

    /// @brief 当前是否打开。
    /// @return true 表示菜单处于打开状态。
    [[nodiscard]] auto is_open() const -> bool { return open_; }

    /// @brief 弹出位置（全局坐标）。
    /// @return 最近一次 `open_at` 记录的锚点；从未打开时为原点。
    [[nodiscard]] auto position() const -> Point { return position_; }

  private:
    std::vector<MenuItem> items_;
    mutable bool open_ = false;
    mutable Point position_{.x = 0.0F, .y = 0.0F};
};

}  // namespace aurora
