// BarChart 控件 demo（图表控件族切片 3）：分组柱状 + 堆叠柱状，含悬停高亮 / 值框与点击回调。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const auto last_tap = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"(no tap yet)"});

    // 分组柱状：两系列并排，类目轴带标签，y 轴带标题与 nice 刻度。
    au::BarChart grouped{au::BarChartProps{
        .series =
            {
                au::ChartSeries{.name = "2024", .values = {12.0, 18.0, 9.0, 24.0, 15.0}},
                au::ChartSeries{.name = "2025", .values = {16.0, 14.0, 20.0, 18.0, 22.0}},
            },
        .categories = {"Mon", "Tue", "Wed", "Thu", "Fri"},
    }};
    grouped.set_axis_y(au::ChartAxisSpec{.label = "sales"});
    grouped.modifier.set(au::Modifier{}.height(200.0F));
    grouped.on_point_tapped = [last_tap](int series_idx, int point_idx) -> void {
        last_tap->set(au::LocalizedString{"tapped series " + std::to_string(series_idx) + " / category " +
                                          std::to_string(point_idx)});
    };

    // 堆叠柱状：同 x 的多系列累加，图例置于底部。
    au::BarChart stacked{au::BarChartProps{
        .series =
            {
                au::ChartSeries{.name = "mobile", .values = {30.0, 42.0, 28.0, 35.0}},
                au::ChartSeries{.name = "desktop", .values = {20.0, 16.0, 24.0, 30.0}},
            },
        .categories = {"Q1", "Q2", "Q3", "Q4"},
        .stacked = true,
        .legend = au::ChartLegendSpec{.visible = true, .position = au::LegendPosition::Bottom},
    }};
    stacked.modifier.set(au::Modifier{}.height(200.0F));

    au::Node root = au::Column{
        GradientTitle{"BarChart widget"},
        gap(12),
        std::move(grouped),
        gap(8),
        au::Text{au::TextProps{.content = au::Reactive{last_tap}}},
        gap(16),
        std::move(stacked),
    };
    return run_demo(Card{std::move(root)}, "BarChart · Aurora Demo", 640.0F, 620.0F);
}
