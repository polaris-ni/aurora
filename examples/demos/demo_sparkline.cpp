// Sparkline 控件 demo（图表控件族切片 4）：最薄图表——无轴 / 无网格 / 无图例 / 无交互，用于卡片内趋势缩览。
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::Sparkline up{au::SparklineProps{.values = {4.0, 6.0, 3.0, 8.0, 5.0, 9.0, 7.0, 11.0}, .line_width = 2.0F}};
    up.set_color(pal::AURORA_OK);
    up.modifier.set(au::Modifier{}.width(220.0F).height(40.0F));

    au::Sparkline down{au::SparklineProps{.values = {9.0, 8.0, 7.5, 5.0, 6.0, 3.0, 4.0, 1.5}}};
    down.set_color(pal::AURORA_DANGER);
    down.set_show_end_dot(false);
    down.modifier.set(au::Modifier{}.width(220.0F).height(40.0F));

    au::Sparkline flat{au::SparklineProps{.values = {5.0, 5.0, 5.0, 5.0}}};
    flat.set_color(pal::AURORA_MUTED);
    flat.modifier.set(au::Modifier{}.width(220.0F).height(40.0F));

    au::Node root = au::Column{
        GradientTitle{"Sparkline widget"},
        gap(12),
        au::Text{"trending up"},
        gap(6),
        std::move(up),
        gap(12),
        au::Text{"trending down (no end dot)"},
        gap(6),
        std::move(down),
        gap(12),
        au::Text{"flat (degenerate domain)"},
        gap(6),
        std::move(flat),
    };
    return run_demo(Card{std::move(root)}, "Sparkline · Aurora Demo", 420.0F, 360.0F);
}
