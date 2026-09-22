// PieChart 控件 demo（图表控件族切片 5）：环图 + 百分比标签 + 极坐标命中 + 扇区点击回调。
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    const auto last_tap = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no tap yet)"});

    au::PieChart donut{au::PieChartProps{
        .sections =
            {
                au::PieSection{.name = "mobile", .value = 45.0},
                au::PieSection{.name = "desktop", .value = 30.0},
                au::PieSection{.name = "tablet", .value = 15.0},
                au::PieSection{.name = "other", .value = 10.0},
            },
        .center_space_ratio = 0.45F,
        .start_angle = -90.0F,
        .show_percentage_labels = true,
        .legend = au::ChartLegendSpec{.visible = true, .position = au::LegendPosition::Right},
    }};
    donut.modifier.set(au::Modifier{}.height(260.0F));
    donut.on_section_tapped = [last_tap](int section_idx) -> void {
        last_tap->set(au::LocalizedString{"tapped section " + std::to_string(section_idx)});
    };

    // 实心饼图（无内孔、无百分比标签）
    au::PieChart pie{au::PieChartProps{
        .sections =
            {
                au::PieSection{.name = "A", .value = 3.0},
                au::PieSection{.name = "B", .value = 2.0},
                au::PieSection{.name = "C", .value = 1.0},
            },
    }};
    pie.modifier.set(au::Modifier{}.height(220.0F));

    au::Node root = au::Column{
        GradientTitle{"PieChart widget"},
        gap(12),
        std::move(donut),
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{last_tap}}},
        gap(16),
        std::move(pie),
    };
    return run_demo(Card{std::move(root)}, "PieChart · Aurora Demo", 640.0F, 640.0F);
}
