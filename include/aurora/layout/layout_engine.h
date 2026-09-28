#pragma once

#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/layout/layout_box.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 布局引擎入口：对 widget 树执行两阶段布局（measure → place）。
///
/// 当前直接驱动 `Widget::layout(constraints, ctx)`（其内部已是两阶段：
/// 先 `layoutImpl` 测量并 `constrain` 出尺寸，再写入 `bounds`），再由
/// `buildBox` 收集成 `LayoutBox` 树（位置 + 尺寸 + 子盒），供命中测试 /
/// 调试快照 / 无头渲染复用（specification/03-layout-render.md §7.1）。
///
/// @note Thread: main-thread only
/// @note Side-effects: mutates layout
/// @note Rebuildable: no
class LayoutEngine {
  public:
    /// @brief 对根 widget 执行布局，结果写入各 widget 的 bounds/size。
    /// @param root 布局起点控件；递归由其 `layout` 驱动整棵子树。
    /// @param constraints 父级下发的尺寸约束（根节点收到的约束）。
    /// @param ctx 构建上下文（主题/环境/缩放读取入口）；缺省 `BuildContext{}` 表示无环境。
    static auto layout(Widget &root, const Constraints &constraints, const BuildContext &ctx = BuildContext{}) -> void {
        root.layout(constraints, ctx);
    }

    /// @brief 布局并产出 `LayoutBox` 树（布局后一次性收集几何结果）。
    /// @param root 布局起点节点；先对其控件执行布局，再收集几何。
    /// @param constraints 根节点收到的尺寸约束。
    /// @param ctx 构建上下文（主题/环境/缩放读取入口）；缺省 `BuildContext{}` 表示无环境。
    /// @return 根节点的布局盒：`rect` 取布局后的 `bounds()`，子盒按 `child_nodes()` 序递归收集
    ///         （虚拟化容器无子盒；`LayoutBox::constraints` 本路径不回填，保持默认值）。
    static auto layout_to_box(Node &root, const Constraints &constraints, const BuildContext &ctx = BuildContext{})
        -> LayoutBox {
        root->layout(constraints, ctx);
        return build_box(root);
    }

    /// @brief 由已布局的 Node 树收集 `LayoutBox` 树（不含约束，仅几何）。
    /// @param root 已布局的起点节点；只读取其 `bounds()` 与子节点，不触发重新测量。
    /// @return 与控件树同形的几何盒；每盒 `rect` 为该节点 bounds，子盒按 `child_nodes()` 序排列。
    static auto build_box(const Node &root) -> LayoutBox {
        LayoutBox box;
        box.rect = root.bounds();
        for (const Node &child : root.widget().child_nodes()) {
            box.children.push_back(build_box(child));
        }
        return box;
    }
};

}  // namespace aurora
