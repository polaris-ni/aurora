#pragma once

// ============================================================================
// 无障碍语义树构建器（widget 层）
// ----------------------------------------------------------------------------
// 本头承载**需要 `Widget` 完整定义**的无障碍语义树构建逻辑：几何盒取值、Name 回退链
// （唯一文本子节点 / 兄弟标签关联）、递归节点与整树构建。
//
// 为何单列在 `widget/` 而非与类型同处 `core/accessibility.h`：语义树构建本质是**对控件树
// 的遍历**（调 `Widget::for_each_child` / `child_nodes()` / `paint_bounds()` / `accessibility_*()`
// 钩子），据此 `core/`（基础层）会反向依赖 `widget/`（组件层），违反「core/ 不依赖任何其他
// aurora 模块」的硬边界。拆分后 `core/accessibility.h` 只保留纯数据类型与指针级钩子，
// 本头承担全部的树上遍历；`core/` 的跨模块依赖因此归零（见 ARCHITECTURE.md §2）。
//
// 依赖方向：widget/ → core/（单向）。本头是 `core/accessibility.h` 的**下游**，反之不成立。
// ============================================================================

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/widget/widget.h"

namespace aurora {

namespace detail {

/// @brief 语义几何盒取值：已绘制优先取绘制遍历写入的绝对盒，否则回退布局累加盒。
///
/// 绘制盒（`Widget::paint_bounds()`，与 `Widget::focus_bounds_` 同源同值）含 Modifier 的
/// padding / border 位移，是屏幕坐标下的真实盒；布局累加盒只累加各级 `Node` 的局部原点，
/// 不带父级 padding 偏移。**优先绘制盒**保证有 present 过的树几何精确；
/// **回退累加盒**保证未绘制（纯布局单测、首帧前查询）时节点仍有非空几何。
/// @note 离屏缓冲（如 `Scroll` 内容）内的后代盒为内容坐标系，与 `paint_bounds()` 同限制。
[[nodiscard]] inline auto accessibility_box(const Widget &w, const Rect &layout_box) -> Rect {
    const Rect painted = w.paint_bounds();
    if (painted.size.width > 0.0F || painted.size.height > 0.0F) {
        return painted;
    }
    return layout_box;
}

/// @brief 直接子节点中的「唯一文本子节点」文本（G24 Name 回退链第三级）。
///
/// 图标 + 文字按钮是常见形态：容器本身无 label，其唯一 `Text` 子节点即读屏应念的内容。
/// 多个文本子节点时**不猜测**（避免把整段内容拼成名字），返回空串交由调用方回落。
[[nodiscard]] inline auto unique_text_child_name(const Widget &w) -> std::string {
    std::string found;
    int text_children = 0;
    w.for_each_child([&found, &text_children](const Widget &child) -> void {
        if (!child.show.get()) {
            return;
        }
        const char *tn = child.type_name();
        if (tn == nullptr) {
            return;
        }
        const std::string_view name{tn};
        if (name != "Text" && name != "RichText" && name != "Label") {
            return;
        }
        ++text_children;
        if (text_children == 1) {
            found = child.accessibility_label();
        }
    });
    return (text_children == 1) ? found : std::string{};
}

/// @brief 兄弟标签关联（G24 Name 回退链第四级；设计 §16.2 #1-C）。
///
/// CheckBox / Switch / Slider 是无内置文本的叶子控件，其可读标签通常是**同容器的兄弟**
/// `Text` / `Label` / `RichText` 节点（如 `Row { Text("启用"), Checkbox() }`）。
/// 第三级「唯一文本子节点」兜底只在标签是**子节点**时命中，对兄弟形态失效。
///
/// 启发式：在父容器的直接子节点中，找到与控件**垂直重叠**且**水平相邻**（间隙 ≈ 0，容差 12 DIP）
/// 的文本兄弟，取几何最近者作为标签。仅对需要标签的控件角色生效，避免对 Button / Generic
/// 等误关联。无父 / 几何缺失（未绘制）时安全回落空串。
///
/// @note RTL 下标签可能在控件右侧；本兜底接受左右两侧的最近者，方向无关。
[[nodiscard]] inline auto sibling_label_name(const Widget &w, const Rect &box) -> std::string {
    const AccessibilityRole role = w.accessibility_role();
    if (role != AccessibilityRole::Checkbox && role != AccessibilityRole::Switch && role != AccessibilityRole::Slider) {
        return std::string{};
    }
    const Widget *parent = w.layout_parent();
    if (parent == nullptr) {
        return std::string{};
    }
    const Rect wb = (box.size.width > 0.0F || box.size.height > 0.0F) ? box : w.paint_bounds();
    if (wb.size.width <= 0.0F || wb.size.height <= 0.0F) {
        return std::string{};  // 几何缺失：不安全关联
    }
    const Point wc{wb.origin.x + wb.size.width * 0.5F, wb.origin.y + wb.size.height * 0.5F};
    constexpr float AURORA_TOL = 12.0F;
    std::string best;
    double best_score = std::numeric_limits<double>::infinity();
    parent->for_each_child([&](const Widget &sib) -> void {
        if (&sib == &w) {
            return;
        }
        const char *tn = sib.type_name();
        if (tn == nullptr) {
            return;
        }
        const std::string_view name{tn};
        if (name != "Text" && name != "RichText" && name != "Label") {
            return;
        }
        const std::string label = sib.accessibility_label();
        if (label.empty()) {
            return;
        }
        const Rect sb = sib.paint_bounds();
        if (sb.size.width <= 0.0F || sb.size.height <= 0.0F) {
            return;
        }
        // 垂直重叠（带容差）：标签与控件应在同一行。
        const bool v_overlap = (sb.origin.y + sb.size.height) >= (wb.origin.y - AURORA_TOL) &&
                               sb.origin.y <= (wb.origin.y + wb.size.height + AURORA_TOL);
        if (!v_overlap) {
            return;
        }
        const Point sc{sb.origin.x + sb.size.width * 0.5F, sb.origin.y + sb.size.height * 0.5F};
        // 水平相邻：标签在左（gap = 控件左 − 标签右）或在右（gap = 标签左 − 控件右）。
        const double gap = (sc.x <= wc.x) ? (wb.origin.x - (sb.origin.x + sb.size.width))
                                          : (sb.origin.x - (wb.origin.x + wb.size.width));
        if (gap < -AURORA_TOL || gap > AURORA_TOL) {
            return;  // 非相邻（间隙过大或重叠过多）
        }
        const double score = std::abs(gap) + std::abs(sc.y - wc.y);
        if (score < best_score) {
            best_score = score;
            best = label;
        }
    });
    return best;
}

/// @brief Name（可访问名）回退链（G24，对标 ARIA accessible name computation）。
///
/// ```
/// name = accessibility_label()                       // 显式标签优先
///      ?: 文本内容（Text / TextInput 的 value）       // 文本类控件的内容即名字
///      ?: 唯一 Text 子节点的文本                      // 图标 + 文字按钮
///      ?: 兄弟标签关联（最近且相邻的文本兄弟）          // CheckBox/Slider 等叶子控件（#1-C）
///      ?: ""                                         // 装饰节点，交由 G23 裁剪忽略
/// ```
/// @note 只对本控件求值，不含子节点递归（回退链第三级是唯一例外，且只在恰好一个文本子节点时生效）。
[[nodiscard]] inline auto resolve_accessibility_name(const Widget &w, AccessibilityRole role) -> std::string {
    if (auto label = w.accessibility_label(); !label.empty()) {
        return label;
    }
    if (role == AccessibilityRole::Text || role == AccessibilityRole::TextInput) {
        if (auto text = w.accessibility_text(); !text.empty()) {
            return std::string{text};
        }
        return w.accessibility_value();
    }
    const std::string sib = sibling_label_name(w, Rect{});
    if (!sib.empty()) {
        return sib;
    }
    return unique_text_child_name(w);
}

/// @brief 递归构建单个节点：几何由 `layout_box` 累加，子节点累加各自 `Node` 局部原点。
/// @param w 当前控件
/// @param layout_box 当前控件按布局累加得到的候选全局盒（未绘制时采用）
/// @param clip_box 父级可见盒（判定 `offscreen` 用；根为其自身盒）
[[nodiscard]] inline auto build_accessibility_node(const Widget &w, const Rect &layout_box, const Rect &clip_box)
    -> AccessibilityNode {
    AccessibilityNode node;
    node.id = w.runtime_id();
    node.role = w.accessibility_role();
    node.actions = default_actions(node.role);
    node.value = w.accessibility_value();
    node.hint = w.accessibility_hint();
    node.bounds = accessibility_box(w, layout_box);
    node.range = w.accessibility_range();
    node.level = w.accessibility_level();

    // 滚动语义（G32）：容器声明了可滚动量即补滚动动作位（三桥共用同一来源）。
    if (const auto scroll = w.accessibility_scroll(); scroll.has_value() && scroll->max > scroll->min) {
        node.actions = node.actions | AccessibilityAction::ScrollDown | AccessibilityAction::ScrollUp;
    }

    // 状态位：控件填自身可知的位，派生位（visible/focusable/offscreen）由共享层统一补。
    node.state = w.accessibility_state();
    node.state.visible = true;  // 已按 show 过滤：进入语义树即可见
    node.state.focusable = node.has_action(AccessibilityAction::Focus);
    const bool zero_sized = node.bounds.size.width <= 0.0F || node.bounds.size.height <= 0.0F;
    node.state.offscreen = zero_sized || !clip_box.intersects(node.bounds);

    // 子节点优先走 `child_nodes()`：其 `Node` 带父写入的局部盒，可累加出真实几何。
    const auto &nodes = w.child_nodes();
    if (!nodes.empty()) {
        for (const Node &child : nodes) {
            // 不可见子节点（`show == false`）与绘制/命中同口径：不入语义树，避免屏幕阅读器
            // 读到视觉上不存在的控件。
            if (!child.widget().show.get()) {
                continue;
            }
            // 累加仍走**布局**原点（而非绘制盒原点）：绘制偏移 Modifier（transform 等）会改变
            // 控件自身的绘制盒，据此累加会把偏移重复计入后代；后代各自再由 accessibility_box
            // 取其精确绘制盒，二者互不干扰。
            const Rect cb = child.bounds();
            const Rect child_box{
                .origin = Point{.x = layout_box.origin.x + cb.origin.x, .y = layout_box.origin.y + cb.origin.y},
                .size = cb.size,
            };
            node.children.push_back(build_accessibility_node(child.widget(), child_box, node.bounds));
        }
    } else {
        // 兜底：虚拟化列表 / 导航栈等容器把子节点存在 `Node` 之外的私有表中，不覆写
        // `child_nodes()`（因而无 Node 局部盒），只经 `for_each_child` 暴露子树。此处与其行为对齐
        // 展开，几何缺失 ⇒ 后代继承本节点盒。（不可把 Node 拷出容器补几何：`Node` 析构会清子节点
        // 的 `layout_parent_`，脏标记传播会断链。）
        w.for_each_child([&node](const Widget &child) -> void {
            if (!child.show.get()) {
                return;
            }
            node.children.push_back(build_accessibility_node(child, node.bounds, node.bounds));
        });
    }

    // Name 回退链需在子节点之后求值（第三级要读子节点文本）。
    node.name = resolve_accessibility_name(w, node.role);
    apply_semantic_pruning(node, w.accessibility_is_semantic());
    return node;
}

/// @brief 递归构建单个节点（根的裁剪盒即自身盒）。
[[nodiscard]] inline auto build_accessibility_node(const Widget &w, const Rect &layout_box) -> AccessibilityNode {
    return build_accessibility_node(w, layout_box, layout_box);
}

}  // namespace detail

/// @brief 递归构建控件树的无障碍视图（含几何）。
///
/// 几何来源见 `detail::accessibility_box`：已绘制取绘制盒、未绘制取布局累加盒。
/// 根节点未绘制时，其盒由 `root.size()`（布局结果）置于原点导出，故调用前应先完成一次布局
/// （`root.layout(constraints, ctx)`）以获得真实尺寸。
/// @note 不依赖 GUI 后端；name / value / hint 由 `Widget::accessibility_*()` 钩子自填。
[[nodiscard]] inline auto build_accessibility_tree(const Widget &root, const Rect &root_box) -> AccessibilityNode {
    return detail::build_accessibility_node(root, root_box);
}

/// @brief 递归构建控件树的无障碍视图（根节点几何置于原点）。
/// @note Side-effects: reads layout/state
[[nodiscard]] inline auto build_accessibility_tree(const Widget &root) -> AccessibilityNode {
    return detail::build_accessibility_node(root, Rect{.origin = Point{}, .size = root.size()});
}

}  // namespace aurora
