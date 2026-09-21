#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 焦点移动方向（specification/05-event-navigation.md §4.1）。
 *
 * - `Forward` / `Backward`：沿 Tab 序前进 / 后退（对应 Tab / Shift+Tab）。
 * - `Up` / `Down` / `Left` / `Right`：方向性焦点移动（按几何最近候选移动；无候选返回 false）。
 */
enum class FocusDirection : std::uint8_t { Forward, Backward, Up, Down, Left, Right };

/**
 * @brief 焦点管理（specification/05-event-navigation.md §4）。单线程下持有当前焦点 widget 与根树引用。
 *
 * 职责：
 * - 记录并切换焦点 widget（`set_focus` / `request_focus` / `clear`）。
 * - 沿 Tab 序移动焦点（`move_focus`，基于 widget 树的 `focusable` + `tabIndex` 顺序遍历）。
 * - 焦点变更时通知相关 widget（`on_focus_change(true/false)`），便于控件重绘聚焦态。
 *
 * 键盘事件经 `EventDispatcher::dispatch(KeyEvent, FocusManager)` 派发到焦点 widget；
 * Tab / Shift+Tab 由派发器识别并转交 `move_focus`。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
class FocusManager {
  public:
    /// @brief 设置根树（move_focus 遍历所需）；可为 nullptr（禁用 Tab 导航）。
    auto set_root(Widget *root) -> void;

    /// @brief 当前焦点 widget（无则 nullptr）。
    [[nodiscard]] auto focused() const -> Widget *;

    /// @brief 指定 widget 当前是否持有焦点。
    [[nodiscard]] auto has_focus(const Widget *w) const -> bool;

    /// @brief 主动请求焦点（等价 set_focus，语义上由 widget 调用）。
    auto request_focus(Widget *w) -> void;

    /// @brief 设置焦点 widget（可为 nullptr 清除）；执行失焦/获焦通知。
    /// @param w widget
    /// @param reason 移动方向（用于扩展；当前仅影响 Tab 序定位，不强制方向性）。
    auto set_focus(Widget *w, FocusDirection reason = FocusDirection::Forward) -> void;

    /// @brief 清除焦点（触发旧 widget 失焦通知）。
    auto clear() -> void;

    /// @brief 焦点变更回调（旧、新 widget；可为空）。
    auto set_on_change(std::function<void(Widget *, Widget *)> cb) -> void;

    /**
     * @brief 沿 Tab 序移动到下一个/上一个可聚焦 widget。
     * @param dir `Forward`/`Backward` 沿 Tab 序循环；`Up`/`Down`/`Left`/`Right` 按几何最近候选移动（无候选返回 false）。
     * @return 是否成功移动焦点（无候选时返回 false）。
     */
    auto move_focus(FocusDirection dir = FocusDirection::Forward) -> bool;

    /// @brief 压入焦点作用域（焦点陷阱）：此后 `move_focus` 的候选集限定在 `subtree` 子树内，
    ///        Tab 循环不逃出（scope 内自然回卷）；同时自动把焦点移入子树内首个可聚焦控件
    ///        （无候选则保持原焦点），并记录打开前焦点供 `pop_scope` 恢复。
    ///
    /// 供模态弹层（Dialog/Popup/Drawer）打开时调用、关闭时配对 `pop_scope`；可嵌套
    /// （多层弹层各自 push/pop，恢复顺序与压栈相反）。`subtree` 为空指针时仅记录焦点快照。
    auto push_scope(Widget *subtree) -> void;

    /// @brief 弹出栈顶焦点作用域：恢复该作用域压入前的焦点（已回收则清除焦点）；空栈为 no-op。
    auto pop_scope() -> void;

    /// @brief 当前焦点作用域深度（0 = 无作用域，Tab 遍历整棵根树）。
    [[nodiscard]] auto scope_depth() const -> std::size_t { return scopes_.size(); }

  private:
    /// @brief 收集根树下所有可聚焦且可见的 widget，按 (tabIndex, 遍历序) 排序。
    static auto collect_focusable(const Widget &root) -> std::vector<Widget *>;

    static auto collect_focusable_impl(const Widget &w, std::vector<Widget *> &out) -> void;

    /// @brief 焦点控件的存活判定：由 shared_ptr 持有且已被回收时返回 nullptr。
    ///
    /// 焦点控件常在自身被重建/回收后仍留在 `focused_` 里（如输入框所在页面被
    /// `push_replacement` 换掉），此后任何按键都会对已释放内存做虚调用。
    /// 与 `HitNode` 同构：构造时探测是否由 `shared_ptr` 持有，栈/成员控件回退为裸指针。
    [[nodiscard]] auto live_focused() const -> Widget *;

    Widget *root_ = nullptr;
    Widget *focused_ = nullptr;
    std::weak_ptr<Widget> focused_guard_;  ///< 生命周期守卫；仅当焦点控件由 shared_ptr 持有时有效
    bool focused_guarded_ = false;  ///< guard 是否关联控制块（区分「空弱引用」与「已失效弱引用」）
    std::function<void(Widget *, Widget *)> on_change_;

    /// @brief 焦点作用域栈项：限定子树 + 压入前的焦点快照（同 `focused_` 的守卫语义）。
    struct ScopeEntry {
        Widget *subtree = nullptr;  ///< 候选限定子树（nullptr = 仅快照，不限定）
        Widget *saved_focus = nullptr;  ///< 压入前的焦点（裸指针视图）
        std::weak_ptr<Widget> saved_guard;  ///< 同 focused_guard_：shared_ptr 持有时判定存活
        bool saved_guarded = false;
    };
    std::vector<ScopeEntry> scopes_;  ///< 作用域栈；栈顶为当前生效作用域
};

/// @brief 派发期间当前焦点管理器（单线程；由 `EventDispatcher` 在派发时设置，退出时复原）。
/// @note 控件 `request_focus()` 读取此值，无需在每控件上持久持有 `FocusManager*`。
[[nodiscard]] auto current_focus_manager() noexcept -> FocusManager *;

/// @brief 设置/复原派发期间的当前焦点管理器（由 `EventDispatcher` 配对调用；嵌套派发须自行保存旧值）。
auto set_current_focus_manager(FocusManager *fm) noexcept -> void;

/// @brief 解析与某控件关联的焦点管理器（无障碍动作 / 交互模拟的统一取用点，G1）。
///
/// `current_focus_manager()` 只在 `EventDispatcher::dispatch` 的派发栈内有效，而读屏桥的
/// provider 回调（UIA Invoke/SetFocus、AT-SPI2 DoAction…）发生在**平台调用栈**里、不在任何
/// 派发栈内 —— 那里 `current_focus_manager()` 恒为 nullptr，`Widget::request_focus()` 会静默
/// no-op、`EventDispatcher::dispatch` 会收到空焦点管理器。故凡「不在派发栈内发起动作」的通道
/// 都必须经本函数取用，而非直接读线程局部。
///
/// 解析顺序：
/// 1. 派发期上下文（`current_focus_manager()`）非空 → 直接复用（与真实焦点状态不脱节）；
/// 2. 否则沿 `layout_parent()` 上溯到控件根，取/建**按根缓存**的进程级兜底实例。
///
/// @param w 目标控件（沿其布局父链上溯定位根）
/// @return 可用焦点管理器；仅在控件根无法解析时返回 nullptr（实践上不会发生）
/// @note Thread: main-thread only
/// @note Side-effects: may lazily create a fallback FocusManager (cached per root)
[[nodiscard]] auto resolve_focus_manager(Widget &w) -> FocusManager *;

}  // namespace aurora
