#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"

namespace aurora::a11y {

/// @brief 快照中的一个节点：语义树的扁平化（先序；父子关系由 `parent_id` 表达）。
/// @note Thread: main-thread only
struct NodeSnapshot {
    std::uint64_t id = 0;       ///< 稳定身份（`Widget::runtime_id()`）
    std::uint64_t parent_id = 0;  ///< 父节点 id（0 = 根）
    AccessibilityNode node;      ///< 语义节点（含 id/state/range 等新字段）
    const Widget *widget = nullptr;  ///< 活指针（仅桥内使用，随快照刷新；不入序列化面）
};

/// @brief 一次语义树投影的完整快照（D9）。
/// @note Thread: main-thread only
struct TreeSnapshot {
    std::vector<NodeSnapshot> flat;                             ///< 先序扁平表
    std::unordered_map<std::uint64_t, std::size_t> by_id;        ///< id → flat 下标
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> children_of;  ///< parent_id → 子 id 序列（先序）

    /// @brief 按 id 查节点；未命中返回 nullptr（控件已销毁 ⇒ 平台侧应答「元素不可用」）。
    [[nodiscard]] auto find(std::uint64_t id) const -> const NodeSnapshot * {
        const auto it = by_id.find(id);
        return (it == by_id.end()) ? nullptr : &flat.at(it->second);
    }
};

/// @brief 字段级变化种类（D11 事件派生的输入）。
enum class FieldChange : std::uint8_t {
    Name,
    Value,
    Hint,
    Bounds,
    State,
    Range,
    Actions,
};

/// @brief 两次快照的差异（D11）。
///
/// 语义：只描述「应让平台感知的变化」，不追求最小编辑脚本——结构整段重排时宁多报
/// `moved` 也不误报 remove+add（读屏焦点稳定性优先）。
struct TreeDiff {
    std::vector<std::uint64_t> added;    ///< 新增节点
    std::vector<std::uint64_t> removed;  ///< 移除节点（控件已销毁）
    std::vector<std::uint64_t> moved;    ///< 同 id 换父或换序
    std::vector<std::pair<std::uint64_t, FieldChange>> updated;  ///< 字段变化
    std::optional<std::uint64_t> focused_id;  ///< 新获焦节点（旧快照未获焦者）；无焦点变化为 nullopt

    [[nodiscard]] auto empty() const -> bool {
        return added.empty() && removed.empty() && moved.empty() && updated.empty() && !focused_id.has_value();
    }
};

namespace detail {

/// @brief 语义树 → 快照扁平表：控件树与语义节点树**同步先序遍历**。
///
/// 二者同序的前提是 `build_accessibility_node` 的子节点枚举（`child_nodes()` 或
/// `for_each_child`）与此处一致——任一处换遍历源都会让 widget 指针错位，故两处必须同时修改。
inline auto flatten_snapshot(const Widget &w, const AccessibilityNode &n, std::uint64_t parent_id, TreeSnapshot &out)
    -> void {
    const std::size_t index = out.flat.size();
    out.flat.push_back(NodeSnapshot{.id = n.id, .parent_id = parent_id, .node = n, .widget = &w});
    out.by_id[n.id] = index;
    out.children_of[parent_id].push_back(n.id);
    if (n.children.size() != w.child_nodes().size()) {
        // 走 `for_each_child` 兜底路径的容器（LazyList 等）：按语义节点顺序重新枚举子控件。
        std::size_t i = 0;
        w.for_each_child([&](const Widget &child) -> void {
            if (i >= n.children.size()) {
                return;
            }
            // 不可见子节点在语义树中被跳过（`build_accessibility_node` 同口径），故此处
            // id 对不上即代表该子控件未入树——不消费索引，继续比对下一个语义节点。
            if (n.children[i].id != child.runtime_id()) {
                return;
            }
            const AccessibilityNode &child_node = n.children[i];
            ++i;
            flatten_snapshot(child, child_node, n.id, out);
        });
        return;
    }
    const auto &nodes = w.child_nodes();
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        flatten_snapshot(nodes[i].widget(), n.children[i], n.id, out);
    }
}

/// @brief 最长公共子序列（同父子序比对，识别「换序」而非 remove+add）。
[[nodiscard]] inline auto lcs_length(const std::vector<std::uint64_t> &a, const std::vector<std::uint64_t> &b)
    -> std::vector<std::uint64_t> {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    // dp[i][j] = a[0..i) 与 b[0..j) 的 LCS 长度；树规模（百级）下 O(n·m) 毫秒级即可。
    std::vector<std::vector<std::size_t>> dp(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            dp[i][j] = (a[i - 1] == b[j - 1]) ? dp[i - 1][j - 1] + 1 : std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }
    std::vector<std::uint64_t> seq;
    std::size_t i = n;
    std::size_t j = m;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            seq.push_back(a[i - 1]);
            --i;
            --j;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            --i;
        } else {
            --j;
        }
    }
    std::reverse(seq.begin(), seq.end());
    return seq;
}

}  // namespace detail

/// @brief 构建语义树快照（D9 拉取式重投影的产出）。
///
/// 与 `build_accessibility_tree` 同参数语义：调用前应先完成一次布局以获得真实几何。
/// @note Thread: main-thread only
/// @note Side-effects: reads layout/state
[[nodiscard]] inline auto build_tree_snapshot(const Widget &root, const Rect &root_box) -> TreeSnapshot {
    TreeSnapshot snap;
    const AccessibilityNode tree = build_accessibility_tree(root, root_box);
    detail::flatten_snapshot(root, tree, 0, snap);
    return snap;
}

/// @brief 构建语义树快照（根几何置于原点）。
[[nodiscard]] inline auto build_tree_snapshot(const Widget &root) -> TreeSnapshot {
    return build_tree_snapshot(root, Rect{.origin = Point{}, .size = root.size()});
}

/// @brief 比较两次快照，产出平台事件派生的输入（D11）。
///
/// 算法：按 parent 分组的子 id 序列做 LCS 识别 moved/added/removed；同 id 同父的节点
/// 逐字段比较产出 updated；焦点位变化单独产出 `focused_id`（平台焦点事件优先级最高）。
/// 纯函数、无平台依赖 —— 表驱动单测全覆盖。
/// @note Thread: main-thread only
/// @note Side-effects: pure
[[nodiscard]] inline auto diff_snapshots(const TreeSnapshot &old_, const TreeSnapshot &new_) -> TreeDiff {
    TreeDiff diff;

    for (const NodeSnapshot &n : new_.flat) {
        if (old_.find(n.id) == nullptr) {
            diff.added.push_back(n.id);
        }
    }
    for (const NodeSnapshot &o : old_.flat) {
        if (new_.find(o.id) == nullptr) {
            diff.removed.push_back(o.id);
        }
    }

    // 同父子序 LCS：未在 LCS 中的同父节点 = 换序（moved）。
    for (const auto &[parent_id, new_children] : new_.children_of) {
        const auto it = old_.children_of.find(parent_id);
        if (it == old_.children_of.end()) {
            continue;
        }
        // 保序（LCS）内的节点不动；LCS 外仍存活者 = 换序（moved），换父者在下一段判定。
        const std::vector<std::uint64_t> keep = detail::lcs_length(it->second, new_children);
        for (const std::uint64_t id : it->second) {
            if (std::ranges::find(keep, id) == keep.end() && new_.find(id) != nullptr) {
                diff.moved.push_back(id);
            }
        }
        for (const std::uint64_t id : new_children) {
            if (std::ranges::find(keep, id) == keep.end() && old_.find(id) != nullptr) {
                diff.moved.push_back(id);
            }
        }
    }

    // 跨父迁移：同 id 换了父节点。
    for (const NodeSnapshot &n : new_.flat) {
        const NodeSnapshot *o = old_.find(n.id);
        if (o == nullptr || o->parent_id == n.parent_id) {
            continue;
        }
        diff.moved.push_back(n.id);
    }

    // 字段级比较（同 id 且未被判为结构变化者仍可能字段变化）。
    for (const NodeSnapshot &n : new_.flat) {
        const NodeSnapshot *o = old_.find(n.id);
        if (o == nullptr) {
            continue;
        }
        const AccessibilityNode &a = o->node;
        const AccessibilityNode &b = n.node;
        if (a.name != b.name) {
            diff.updated.emplace_back(n.id, FieldChange::Name);
        }
        if (a.value != b.value) {
            diff.updated.emplace_back(n.id, FieldChange::Value);
        }
        if (a.hint != b.hint) {
            diff.updated.emplace_back(n.id, FieldChange::Hint);
        }
        if (a.bounds.origin.x != b.bounds.origin.x || a.bounds.origin.y != b.bounds.origin.y ||
            a.bounds.size.width != b.bounds.size.width || a.bounds.size.height != b.bounds.size.height) {
            diff.updated.emplace_back(n.id, FieldChange::Bounds);
        }
        const bool range_changed = a.range.has_value() != b.range.has_value() ||
                                   (a.range.has_value() &&
                                    (a.range->min != b.range->min || a.range->max != b.range->max ||
                                     a.range->step != b.range->step || a.range->value != b.range->value));
        if (range_changed) {
            diff.updated.emplace_back(n.id, FieldChange::Range);
        }
        if (a.actions != b.actions) {
            diff.updated.emplace_back(n.id, FieldChange::Actions);
        }
        if (a.state.focused != b.state.focused || a.state.checked != b.state.checked ||
            a.state.selected != b.state.selected || a.state.read_only != b.state.read_only ||
            a.state.visible != b.state.visible || a.state.focusable != b.state.focusable ||
            a.state.offscreen != b.state.offscreen || a.state.disabled != b.state.disabled ||
            a.state.checkable != b.state.checkable || a.state.expanded != b.state.expanded ||
            a.state.expandable != b.state.expandable || a.state.multiline != b.state.multiline ||
            a.state.password != b.state.password) {
            diff.updated.emplace_back(n.id, FieldChange::State);
        }
        if (!a.state.focused && b.state.focused) {
            diff.focused_id = n.id;
        }
    }

    // 去重（同一 id 可能因换父 + 换序被判两次 moved）。
    std::ranges::sort(diff.moved);
    const auto moved_unique = std::ranges::unique(diff.moved);
    diff.moved.erase(moved_unique.begin(), moved_unique.end());
    return diff;
}

}  // namespace aurora::a11y
