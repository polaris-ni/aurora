#pragma once

// ============================================================
// E2E 场景：Column 容器（examples/demos/scenes/scene_column.h）
// ------------------------------------------------------------
// 场景头与组件 demo **同源**：`demo_column.cpp` 的 UI 构建抽到此处（inline 构建函数返回根
// `Node`），demo 退化为「薄 `main()` + `run_demo(build_scene(), ...)`」，E2E 用例 `#include`
// 同一份构建函数——仓库内不存在第二份复制粘贴的组件 UI 代码。
//
// 抽取是**按需**的：只有被 E2E 实际引用的组件才建场景头，未被引用的 demo 源码零改动。
//
// 确定性渲染契约（进入 golden / 像素断言用例集的前提，违反者不得进入该集合）：
//   - 不依赖墙钟时间、不依赖随机数（本场景无随机源，无需固定种子）
//   - 无动画；捕获前无需推进到静止态
//   - 含文本（GradientTitle 与说明标签）：进入 golden 比对须按字体依赖单列更大的差异
//     像素预算（字体回退是跨驱动像素漂移的主因）
// ============================================================

#include "demo_common.h"

namespace aurora::demo_scenes {

/// @brief 场景：Column 纵向线性布局，两块 240×160 面板分别演示主轴 Start 与主轴+交叉轴 Center
///          对齐（对应 `demo_column` 的完整界面，含 Card 外壳与标题文本）。
///
/// 选它作「容器型」代表形态：Column 是全库使用最广的容器，主轴/交叉轴对齐是布局回归的
/// 高频敏感面；面板几何为编译期常量，像素断言可解析给出期望区域。
///
/// 契约：根为 `Card`（含 GradientTitle 与说明文本），画布 560×560（与 demo 一致）。
[[nodiscard]] inline auto build_column() -> Node {
    au::Column start{
        au::ColumnProps{.children = {au::Text{au::LocalizedString{"A"}}, au::Text{au::LocalizedString{"B"}}},
                        .flex = au::Flex{.main_axis = au::MainAxisAlignment::Start}}};
    start.modifier.set(
        au::Modifier{}.size(240.0F, 160.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));

    au::Column center{au::ColumnProps{
        .children = {au::Text{au::LocalizedString{"A"}}, au::Text{au::LocalizedString{"B"}}},
        .flex = au::Flex{.main_axis = au::MainAxisAlignment::Center, .cross_axis = au::CrossAxisAlignment::Center}}};
    center.modifier.set(
        au::Modifier{}.size(240.0F, 160.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{"Column widget"},
        gap(12),
        au::Text{au::LocalizedString{"Main axis Start (top aligned)"}},
        std::move(start),
        gap(12),
        au::Text{au::LocalizedString{"Main axis + cross axis Center"}},
        std::move(center),
    };
    return Card{std::move(root)};
}

}  // namespace aurora::demo_scenes
