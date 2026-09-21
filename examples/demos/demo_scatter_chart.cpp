// ScatterChart 控件 demo（图表控件族切片 6）：双系列散点 + 双数值轴 + 最近点命中与十字准线。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const auto last_tap = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no tap yet)"});

    au::ScatterSeries a{};
    a.name = "A";
    a.points = {
        {.x = 1.0, .y = 2.0}, {.x = 2.5, .y = 4.0}, {.x = 4.0, .y = 3.0}, {.x = 5.5, .y = 6.0}, {.x = 7.0, .y = 5.0}};

    au::ScatterSeries b{};
    b.name = "B";
    b.points = {{.x = 1.5, .y = 1.0}, {.x = 3.0, .y = 2.5}, {.x = 5.0, .y = 4.5}, {.x = 6.5, .y = 3.5}};
    b.dot_radius = 5.0F;

    au::ScatterChart chart{au::ScatterChartProps{.series = {a, b}}};
    chart.set_axis_x(au::ChartAxisSpec{.label = "x", .tick_count = 5});
    chart.set_axis_y(au::ChartAxisSpec{.label = "y", .tick_count = 5});
    chart.modifier.set(au::Modifier{}.height(240.0F));
    chart.on_point_tapped = [last_tap](int series_idx, int point_idx) -> void {
        last_tap->set(au::LocalizedString{"tapped series " + std::to_string(series_idx) + " / point " +
                                          std::to_string(point_idx)});
    };

    au::Node root = au::Column{
        GradientTitle{"ScatterChart widget"},
        gap(12),
        std::move(chart),
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{last_tap}}},
    };
    return run_demo(Card{std::move(root)}, "ScatterChart · Aurora Demo", 640.0F, 420.0F);
}
