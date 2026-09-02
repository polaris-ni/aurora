// Scroll 控件 demo：可滚动区域（滚轮滚动）。
#include "demo_common.h"

auto main() -> int {
    std::vector<au::Node> lines;
    for (int i = 0; i < 40; ++i) {
        au::Text l{ au::LocalizedString{ "line " + std::to_string(i) } };
        l.modifier.set(au::Modifier{}.padding(6.0f).background(pal::AURORA_SURFACE).border(1.0f, pal::AURORA_BORDER));
        lines.emplace_back(std::move(l));
    }

    au::Scroll scroll{ au::Column{ au::ColumnProps{ .children = std::move(lines) } } };
    scroll.modifier.set(au::Modifier{}.size(360.0f, 240.0f).border(1.0f, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{ "Scroll widget" },
        gap(12),
        au::Text{ au::LocalizedString{ "Scrollable area (wheel scroll)" } },
        std::move(scroll),
    };
    return run_demo(Card{ std::move(root) }, "Scroll · Aurora Demo", 520.0f, 420.0f);
}
