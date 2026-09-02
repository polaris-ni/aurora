// Column 控件 demo：纵向线性布局，演示主轴/交叉轴对齐。
#include "demo_common.h"

auto main() -> int {
    au::Column start{ au::ColumnProps{
        .children = { au::Text{ au::LocalizedString{ "A" } }, au::Text{ au::LocalizedString{ "B" } } },
        .flex = au::Flex{ .main_axis = au::MainAxisAlignment::Start } } };
    start.modifier.set(
        au::Modifier{}.size(240.0f, 160.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Column center{ au::ColumnProps{
        .children = { au::Text{ au::LocalizedString{ "A" } }, au::Text{ au::LocalizedString{ "B" } } },
        .flex =
            au::Flex{ .main_axis = au::MainAxisAlignment::Center, .cross_axis = au::CrossAxisAlignment::Center } } };
    center.modifier.set(
        au::Modifier{}.size(240.0f, 160.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{ "Column 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "主轴 Start（顶端排列）" } },
        std::move(start),
        gap(12),
        au::Text{ au::LocalizedString{ "主轴 + 交叉轴 Center" } },
        std::move(center),
    };
    return run_demo(Card{ std::move(root) }, "Column · Aurora Demo", 560.0f, 560.0f);
}
