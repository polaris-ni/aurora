// Row 控件 demo：水平线性布局，演示主轴/交叉轴对齐。
#include "demo_common.h"

auto main() -> int {
    au::Row start{ au::RowProps{
        .children = { au::Text{ au::LocalizedString{ "A" } }, au::Text{ au::LocalizedString{ "B" } } },
        .flex = au::Flex{ .main_axis = au::MainAxisAlignment::Start, .cross_axis = au::CrossAxisAlignment::Center } } };
    start.modifier.set(
        au::Modifier{}.size(280.0f, 80.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Row space_between{ au::RowProps{ .children = { au::Text{ au::LocalizedString{ "Left" } },
                                                       au::Text{ au::LocalizedString{ "Middle" } },
                                                       au::Text{ au::LocalizedString{ "Right" } } },
                                         .flex = au::Flex{ .main_axis = au::MainAxisAlignment::SpaceBetween,
                                                           .cross_axis = au::CrossAxisAlignment::Center } } };
    space_between.modifier.set(
        au::Modifier{}.size(280.0f, 80.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{ "Row widget" },
        gap(12),
        au::Text{ au::LocalizedString{ "Main axis Start" } },
        std::move(start),
        gap(12),
        au::Text{ au::LocalizedString{ "Main axis SpaceBetween" } },
        std::move(space_between),
    };
    return run_demo(Card{ std::move(root) }, "Row · Aurora Demo", 560.0f, 460.0f);
}
