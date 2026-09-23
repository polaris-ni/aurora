#pragma once

// ============================================================
// E2E 场景：纯色块（examples/demos/scenes/scene_solid_rect.h）
// ------------------------------------------------------------
// 场景头与组件 demo **同源**：`demo_<组件>.cpp` 的 UI 构建抽到此处（inline 构建函数返回根
// `Node`），demo 退化为「薄 `main()` + `run_demo(build_scene(), ...)`」，E2E 用例 `#include`
// 同一份构建函数——仓库内不存在第二份复制粘贴的组件 UI 代码。
//
// 抽取是**按需**的：只有被 E2E 实际引用的组件才建场景头，未被引用的 demo 源码零改动。
//
// 确定性渲染契约（进入 golden / 像素断言用例集的前提，违反者不得进入该集合）：
//   - 不依赖墙钟时间、不依赖随机数（本场景无随机源，无需固定种子）
//   - 无动画；捕获前无需推进到静止态
//   - 尺寸与配色为编译期常量，跨平台逐位一致
// ============================================================

#include <memory>

#include "aurora/aurora.h"

namespace aurora::demo_scenes {

/// @brief 场景：320×200 画布左右各一块纯色（左红右蓝），无文本、无圆角。
///
/// 选它作首个 E2E 冒烟场景的理由：**无文本**（字体回退是跨驱动像素漂移的主因），且两块
/// 纯色区域中心的期望 RGBA 可解析给出，无需先建 golden 基线即可做像素断言。
///
/// 契约：根节点尺寸 320×200，左半区 `Color{220, 40, 40, 255}`、右半区 `Color{40, 40, 220, 255}`。
[[nodiscard]] inline auto build_solid_rect() -> Node {
    const auto left = std::make_shared<Row>();
    left->modifier.set(Modifier{}.size(160.0F, 200.0F).background(Color{220, 40, 40, 255}));

    const auto right = std::make_shared<Row>();
    right->modifier.set(Modifier{}.size(160.0F, 200.0F).background(Color{40, 40, 220, 255}));

    return Node{Row{Node{left}, Node{right}}};
}

}  // namespace aurora::demo_scenes
