#pragma once

// ============================================================
// E2E 场景：Scroll 滚动容器（examples/demos/scenes/scene_scroll.h）
// ------------------------------------------------------------
// 场景头与组件 demo **同源**：`demo_scroll.cpp` 的 UI 构建抽到此处（inline 构建函数返回根
// `Node`），demo 退化为「薄 `main()` + `run_demo(build_scene(), ...)`」，E2E 用例 `#include`
// 同一份构建函数——仓库内不存在第二份复制粘贴的组件 UI 代码。
//
// 确定性渲染契约（进入 golden / 像素断言用例集的前提，违反者不得进入该集合）：
//   - 不依赖墙钟时间、不依赖随机数（本场景无随机源，无需固定种子）
//   - 无动画；偏移恒为 0（静止态即构建态），捕获前无需额外推进
//   - 含文本（GradientTitle、说明标签与 40 行列表项）：进入 golden 比对须按字体依赖
//     单列更大的差异像素预算（字体回退是跨驱动像素漂移的主因）
// ============================================================

#include <string>
#include <vector>

#include "demo_common.h"

namespace aurora::demo_scenes {

/// @brief 场景：360×240 滚动视口内 40 行列表项（内容远高于视口，初始偏移 0；对应
///          `demo_scroll` 的完整界面，含 Card 外壳与标题文本）。
///
/// 选它作「含滚动」代表形态：滚动是交互流 E2E（滚轮/拖拽滚动）与脏区刷新的高频敏感面，
/// 行项背景/边框几何为常量，行项内文本按字体依赖单列预算。
///
/// 契约：根为 `Card`，画布 520×420（与 demo 一致）；构建后即静止态，偏移 0。
[[nodiscard]] inline auto build_scroll() -> Node {
    std::vector<au::Node> lines;
    for (int i = 0; i < 40; ++i) {
        au::Text l{au::LocalizedString{"line " + std::to_string(i)}};
        l.modifier.set(au::Modifier{}.padding(6.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
        lines.emplace_back(std::move(l));
    }

    au::Scroll scroll{au::Column{au::ColumnProps{.children = std::move(lines)}}};
    scroll.modifier.set(au::Modifier{}.size(360.0F, 240.0F).border(1.0F, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{"Scroll widget"},
        gap(12),
        au::Text{au::LocalizedString{"Scrollable area (wheel scroll)"}},
        std::move(scroll),
    };
    return Card{std::move(root)};
}

}  // namespace aurora::demo_scenes
