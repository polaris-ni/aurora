#pragma once

// ============================================================
// E2E 场景：Dismissible 拖动消除列表（examples/demos/scenes/scene_dismissible.h）
// ------------------------------------------------------------
// 场景头与组件 demo **同源**：`demo_dismissible.cpp` 的 UI 构建抽到此处（inline 构建函数
// 返回根 `Node`）；该 demo 属「使用 `Application` 装配」的形态——抽取只承载 UI 构建，
// `Application` 装配留在 demo 的 `main()` 内，E2E 用例以同一构建函数取得根节点后经
// E2E 驱动内核自行推进（内核每帧 tick 含手势计时，覆盖 Dismissible 的 spring 推进）。
//
// 确定性渲染契约（进入 golden / 像素断言用例集的前提，违反者不得进入该集合）：
//   - 不依赖墙钟时间、不依赖随机数（本场景无随机源，无需固定种子）
//   - 无自动动画：spring 仅在拖拽交互后触发，构建完成即静止态，捕获前无需额外推进
//   - 含文本（GradientTitle、说明标签与卡片标签）：进入 golden 比对须按字体依赖单列
//     更大的差异像素预算（字体回退是跨驱动像素漂移的主因）
// ============================================================

#include <memory>
#include <string>

#include "aurora/widget/dismissible.h"
#include "demo_common.h"

namespace aurora::demo_scenes {

/// @brief 场景：三张可水平拖出的卡片（第三张注册 on_dismissed 自定义回调，演示「回调
///          接管默认摘除」扩展点；对应 `demo_dismissible` 的完整界面）。
///
/// 选它作「含 Application 装配」代表形态：Dismissible 的手势/弹簧链路是交互流 E2E 的
/// 高价值靶子（跟手 1:1 映射、阈值裁决、摘除重排），卡片几何与配色为常量。
///
/// 契约：根为 Column（Card 外壳在子级），画布 520×420（与 demo 一致）；构建后即静止态。
[[nodiscard]] inline auto build_dismissible() -> Node {
    auto make_card = [](const std::string &label, au::Color tint, bool custom_cb) -> au::Node {
        au::Text title{label};
        title.modifier.set(au::Modifier{}.background(tint).size(280.0F, 56.0F));
        auto dis = std::make_shared<au::Dismissible>(au::Node{std::move(title)});
        if (custom_cb) {
            // 自定义回调接管默认摘除：这里只记录（真实场景多为删除数据后重建子树）。
            // 回调转入 std::function（on_dismissed），本检查对可调用对象一律判「不应抛出」；
            // 体内只走日志通道，其格式化分配即唯一抛出面。
            // NOLINTBEGIN(bugprone-exception-escape)
            dis->on_dismissed(
                [label]() -> void { AURORA_LOG_INFO("demo", "[demo_dismissible] custom dismissed: ", label); });
            // NOLINTEND(bugprone-exception-escape)
        }
        return au::Node{dis};
    };

    au::Column list = au::Column{
        au::ColumnProps{
            .children =
                {
                    make_card("swipe me ->", pal::AURORA_PRIMARY_SOFT, false),
                    make_card("swipe away ->", pal::AURORA_OK, false),
                    make_card("custom callback", pal::AURORA_ACCENT, true),
                },
        },
    };
    auto list_ptr = std::make_shared<au::Column>(std::move(list));

    au::Node root = au::Column{
        au::ColumnProps{
            .children =
                {
                    GradientTitle{"Dismissible"},
                    gap(12),
                    au::Text{"Drag a card horizontally; release past half to dismiss."},
                    gap(16),
                    Card{au::Node{list_ptr}},
                },
        },
    };
    return root;
}

}  // namespace aurora::demo_scenes
