#pragma once

#include "aurora/core/types.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_box.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 布局引擎入口：对 widget 树执行两阶段布局（measure → place）。
 *
 * 当前直接驱动 `Widget::layout(constraints, ctx)`（其内部已是两阶段：
 * 先 `layoutImpl` 测量并 `constrain` 出尺寸，再写入 `bounds`），再由
 * `buildBox` 收集成 `LayoutBox` 树（位置 + 尺寸 + 子盒），供命中测试 /
 * 调试快照 / 无头渲染复用（specification/03-layout-render.md §7.1）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: mutates layout
 * @note Rebuildable: no
 */
class LayoutEngine {
  public:
    /// @brief 对根 widget 执行布局，结果写入各 widget 的 bounds/size。
    static auto layout(Widget &root, const Constraints &constraints, const BuildContext &ctx = BuildContext{}) -> void {
        root.layout(constraints, ctx);
    }

    /// @brief 布局并产出 `LayoutBox` 树（布局后一次性收集几何结果）。
    static auto layout_to_box(Node &root, const Constraints &constraints, const BuildContext &ctx = BuildContext{})
        -> LayoutBox {
        root->layout(constraints, ctx);
        return build_box(root);
    }

    /// @brief 由已布局的 Node 树收集 `LayoutBox` 树（不含约束，仅几何）。
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
