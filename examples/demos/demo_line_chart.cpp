// LineChart 控件 demo（图表控件族切片 4）：多系列折线 + 数据点 + 悬停十字准线 / 值框 + 图例联动。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const auto last_tap = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no tap yet)"});

    au::LineChart chart{au::LineChartProps{
        .series =
            {
                au::ChartSeries{.name = "cpu", .values = {12.0, 18.0, 9.0, 24.0, 15.0, 20.0}},
                au::ChartSeries{.name = "mem", .values = {8.0, 10.0, 14.0, 12.0, 18.0, 16.0}},
            },
        .categories = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
    }};
    chart.set_axis_y(au::ChartAxisSpec{.label = "usage (%)", .tick_count = 5});
    chart.modifier.set(au::Modifier{}.height(220.0F));
    chart.on_point_tapped = [last_tap](int series_idx, int point_idx) -> void {
        last_tap->set(au::LocalizedString{"tapped series " + std::to_string(series_idx) + " / point " +
                                          std::to_string(point_idx)});
    };

    // 迷你趋势线：无轴 / 无图例，用于卡片内的缩览
    au::Sparkline spark{au::SparklineProps{.values = {4.0, 6.0, 3.0, 8.0, 5.0, 9.0, 7.0, 11.0}, .line_width = 2.0F}};
    spark.set_color(pal::AURORA_PRIMARY);
    spark.modifier.set(au::Modifier{}.width(200.0F).height(40.0F));

    au::Node root = au::Column{
        GradientTitle{"LineChart widget"},
        gap(12),
        std::move(chart),
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{last_tap}}},
        gap(16),
        au::Text{"Sparkline (no axes / no legend)"},
        gap(8),
        std::move(spark),
    };
    return run_demo(Card{std::move(root)}, "LineChart · Aurora Demo", 640.0F, 560.0F);
}
