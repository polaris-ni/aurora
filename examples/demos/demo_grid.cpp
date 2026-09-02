// Grid 控件 demo：多列网格布局。
#include "demo_common.h"

auto main() -> int {
    std::vector<au::Node> cells;
    for (int i = 0; i < 9; ++i) {
        au::Text c{ au::LocalizedString{ "cell " + std::to_string(i) } };
        c.modifier.set(au::Modifier{}.padding(14.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));
        cells.emplace_back(std::move(c));
    }

    au::Node root = au::Column{
        GradientTitle{ "Grid widget" },
        gap(12),
        au::Text{ au::LocalizedString{ "3-column grid" } },
        au::Grid{ au::GridProps{ .children = std::move(cells), .columns = 3 } },
    };
    return run_demo(Card{ std::move(root) }, "Grid · Aurora Demo", 520.0f, 420.0f);
}
