#pragma once

// ============================================================
// E2E 场景注册表（examples/demos/scenes/scene_registry.h）
// ------------------------------------------------------------
// 场景库**自带**的枚举面：场景 id 与组件名一一对应（唯一例外 `solid_rect` 是冒烟特意
// 构造的最小场景，无 demo 对应体；其余 id 即被按需抽取的 `demo_<组件>`）。
//
// 消费方是 examples/demos/scene_tool.cpp（--list / --render），供筛选与批量执行、
// golden 基线的软件 SSOT 生成与场景内容人工核对；**不占用测试 runner 的 --list**——
// 那是「用例/套件」面，与「场景」面是两个不同的 CLI 面，不得混淆。
// ============================================================

#include <vector>

#include "scenes/scene_column.h"
#include "scenes/scene_dismissible.h"
#include "scenes/scene_scroll.h"
#include "scenes/scene_solid_rect.h"

namespace aurora::demo_scenes {

/// @brief 单条场景元数据：id、构建函数、推荐画布尺寸、来源 demo 标题（非 demo 场景为空串）。
struct SceneEntry {
    const char *id;
    Node (*build)();
    float width;
    float height;
    const char *title;
};

/// @brief 全部可用场景（随 scenes/ 下场景头增长）；顺序即枚举顺序，稳定可依赖。
[[nodiscard]] inline auto scene_registry() -> const std::vector<SceneEntry> & {
    static const std::vector<SceneEntry> REGISTRY = {
        {.id = "solid_rect", .build = build_solid_rect, .width = 320.0F, .height = 200.0F, .title = ""},
        {.id = "column", .build = build_column, .width = 560.0F, .height = 560.0F, .title = "Column · Aurora Demo"},
        {.id = "scroll", .build = build_scroll, .width = 520.0F, .height = 420.0F, .title = "Scroll · Aurora Demo"},
        {.id = "dismissible",
         .build = build_dismissible,
         .width = 520.0F,
         .height = 420.0F,
         .title = "Dismissible · Aurora Demo"},
    };
    return REGISTRY;
}

}  // namespace aurora::demo_scenes
