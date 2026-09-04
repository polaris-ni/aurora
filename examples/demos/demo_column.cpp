// Column 控件 demo：纵向线性布局，演示主轴/交叉轴对齐。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
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
    return run_demo(Card{std::move(root)}, "Column · Aurora Demo", 560.0F, 560.0F);
}