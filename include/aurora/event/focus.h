#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 焦点移动方向（specification/05-event-navigation.md §4.1）。
 *
 * - `Forward` / `Backward`：沿 Tab 序前进 / 后退（对应 Tab / Shift+Tab）。
 * - `Up` / `Down` / `Left` / `Right`：方向性焦点移动（供后续布局感知导航扩展）。
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
     * @param dir `Forward` 前进、`Backward` 后退；其余方向暂退化为 Forward。
     * @return 是否成功移动焦点（无候选时返回 false）。
     */
    auto move_focus(FocusDirection dir = FocusDirection::Forward) -> bool;

  private:
    /// @brief 收集根树下所有可聚焦且可见的 widget，按 (tabIndex, 遍历序) 排序。
    static auto collect_focusable(Widget &root) -> std::vector<Widget *>;

    static auto collect_focusable_impl(const Widget &w, std::vector<Widget *> &out) -> void;

    /// @brief 焦点控件的存活判定：由 shared_ptr 持有且已被回收时返回 nullptr。
    ///
    /// 焦点控件常在自身被重建/回收后仍留在 `m_focused` 里（如输入框所在页面被
    /// `push_replacement` 换掉），此后任何按键都会对已释放内存做虚调用。
    /// 与 `HitNode` 同构：构造时探测是否由 `shared_ptr` 持有，栈/成员控件回退为裸指针。
    [[nodiscard]] auto live_focused() const -> Widget *;

    Widget *m_root = nullptr;
    Widget *m_focused = nullptr;
    std::weak_ptr<Widget> m_focused_guard; ///< 生命周期守卫；仅当焦点控件由 shared_ptr 持有时有效
    bool m_focused_guarded = false;        ///< guard 是否关联控制块（区分「空弱引用」与「已失效弱引用」）
    std::function<void(Widget *, Widget *)> m_on_change;
};

/// @brief 派发期间当前焦点管理器（单线程；由 `EventDispatcher` 在派发时设置，退出时复原）。
/// @note 控件 `request_focus()` 读取此值，无需在每控件上持久持有 `FocusManager*`。
[[nodiscard]] auto current_focus_manager() noexcept -> FocusManager *;

/// @brief 设置/复原派发期间的当前焦点管理器（由 `EventDispatcher` 配对调用；嵌套派发须自行保存旧值）。
auto set_current_focus_manager(FocusManager *fm) noexcept -> void;

} // namespace aurora
