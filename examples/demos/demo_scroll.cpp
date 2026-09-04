// Scroll 控件 demo：可滚动区域（滚轮滚动）。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
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
    return run_demo(Card{std::move(root)}, "Scroll · Aurora Demo", 520.0F, 420.0F);
}