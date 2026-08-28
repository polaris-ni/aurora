#include "aurora/event/focus.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aurora {

namespace {
/// @brief 派发期间当前焦点管理器的线程本地槽位（嵌套派发由调用方配对复原）。
///
/// 以「函数内 `thread_local` + 访问器」提供，而非命名空间级可变全局变量：该状态是
/// **派发上下文**（进入派发时设置、退出时复原），且指向的 `FocusManager` 必须可写
/// （`set_focus` / `move_focus` 会改焦点），因此既无法改为 const，也不该暴露为全局。
[[nodiscard]] auto focus_manager_slot() noexcept -> FocusManager *& {
    thread_local FocusManager *slot = nullptr;
    return slot;
}
} // namespace

auto current_focus_manager() noexcept -> FocusManager * { return focus_manager_slot(); }

auto set_current_focus_manager(FocusManager *fm) noexcept -> void { focus_manager_slot() = fm; }

auto FocusManager::set_root(Widget *root) -> void { m_root = root; }

auto FocusManager::live_focused() const -> Widget * {
    if (m_focused_guarded) {
        return m_focused_guard.lock().get(); // 由 shared_ptr 持有：回收后为 nullptr
    }
    return m_focused; // 栈/成员对象：生命周期由持有者保证
}

auto FocusManager::focused() const -> Widget * { return live_focused(); }

auto FocusManager::has_focus(const Widget *w) const -> bool { return w != nullptr && w == live_focused(); }

auto FocusManager::request_focus(Widget *w) -> void { set_focus(w, FocusDirection::Forward); }

auto FocusManager::set_focus(Widget *w, FocusDirection /*reason*/) -> void {
    // 与已回收的旧焦点比较须用存活视图，否则「新焦点恰好复用了同一地址」会被误判为无变化。
    Widget *old = live_focused();
    if (w == old) {
        return;
    }
    m_focused = w;
    // 记录生命周期守卫：能 lock 成功即说明该控件由 shared_ptr 持有（此刻它必然存活）。
    m_focused_guard = (w != nullptr) ? w->weak_from_this() : std::weak_ptr<Widget>{};
    m_focused_guarded = (w != nullptr) && (m_focused_guard.lock() != nullptr);
    if (old != nullptr) {
        old->on_focus_change(false);
    }
    if (w != nullptr) {
        w->on_focus_change(true);
    }
    if (m_on_change) {
        m_on_change(old, w);
    }
}

auto FocusManager::clear() -> void { set_focus(nullptr); }

auto FocusManager::set_on_change(std::function<void(Widget *, Widget *)> cb) -> void { m_on_change = std::move(cb); }

auto FocusManager::move_focus(FocusDirection dir) -> bool {
    if (m_root == nullptr) {
        return false;
    }
    std::vector<Widget *> candidates = collect_focusable(*m_root);
    if (candidates.empty()) {
        return false;
    }

    // 焦点控件可能已被回收：一律经存活视图取用，避免下方 focus_bounds() 打到已释放内存。
    Widget *const cur_focus = live_focused();

    // Tab 序导航（Forward/Backward）
    if (dir == FocusDirection::Forward || dir == FocusDirection::Backward) {
        const bool backward = (dir == FocusDirection::Backward);
        if (cur_focus == nullptr) {
            set_focus(candidates.front(), dir);
            return true;
        }
        const auto it = std::ranges::find(candidates, cur_focus);
        const size_t idx = it == candidates.end() ? 0 : static_cast<size_t>(it - candidates.begin());
        const size_t n = candidates.size();
        const size_t next = backward ? (idx + n - 1) % n : (idx + 1) % n;
        set_focus(candidates[next], dir); // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        return true;
    }

    // 方向键导航（Up/Down/Left/Right）：按几何位置找最近候选
    if (cur_focus == nullptr) {
        set_focus(candidates.front(), dir);
        return true;
    }

    const auto [origin, size] = cur_focus->focus_bounds();
    const float cx = origin.x + (size.width * 0.5f);
    const float cy = origin.y + (size.height * 0.5f);

    Widget *best = nullptr;
    float best_score = std::numeric_limits<float>::max();

    for (Widget *w : candidates) {
        if (w == cur_focus) {
            continue;
        }
        const auto [r_origin, r_size] = w->focus_bounds();
        const float wx = r_origin.x + (r_size.width * 0.5f);
        const float wy = r_origin.y + (r_size.height * 0.5f);

        // 检查候选是否在指定方向上
        bool in_direction = false;
        float primary_dist = 0.0f;
        float secondary_dist = 0.0f;

        switch (dir) {
        case FocusDirection::Up:
            in_direction = (wy < cy);
            primary_dist = cy - wy;
            secondary_dist = std::abs(wx - cx);
            break;
        case FocusDirection::Down:
            in_direction = (wy > cy);
            primary_dist = wy - cy;
            secondary_dist = std::abs(wx - cx);
            break;
        case FocusDirection::Left:
            in_direction = (wx < cx);
            primary_dist = cx - wx;
            secondary_dist = std::abs(wy - cy);
            break;
        case FocusDirection::Right:
            in_direction = (wx > cx);
            primary_dist = wx - cx;
            secondary_dist = std::abs(wy - cy);
            break;
        default: break;
        }

        if (!in_direction) {
            continue;
        }

        // 评分：主方向距离 + 垂直偏移惩罚（2x）
        const float score = primary_dist + (secondary_dist * 2.0f);
        if (score < best_score) {
            best_score = score;
            best = w;
        }
    }

    if (best != nullptr) {
        set_focus(best, dir);
        return true;
    }
    return false;
}

auto FocusManager::collect_focusable(Widget &root) -> std::vector<Widget *> {
    std::vector<Widget *> out;
    collect_focusable_impl(root, out);
    std::ranges::stable_sort(out,
                             [](const Widget *a, const Widget *b) -> bool { return a->tab_index() < b->tab_index(); });
    return out;
}

auto FocusManager::collect_focusable_impl(const Widget &w, std::vector<Widget *> &out) -> void {
    if (!w.show.get()) {
        return; // 不可见控件不参与焦点序
    }
    if (w.focusable()) {
        // 子控件遍历（`for_each_child` / `child_nodes`）只暴露 const 视图，但焦点候选必须存为
        // 可写 `Widget*`（`move_focus` 后续的 `set_focus` 要调用 `on_focus_change` 改控件焦点态）。
        // 控件本身并非 const 对象（根来自 `FocusManager::set_root(Widget*)`），此处去 const 不改变
        // 任何对象，是遍历 API 受限下唯一且必要的转换点；遍历全程只读，不修改子控件。
        out.push_back(const_cast<Widget *>(&w)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    w.for_each_child([&](const Widget &child) -> void { collect_focusable_impl(child, out); });
}

} // namespace aurora
