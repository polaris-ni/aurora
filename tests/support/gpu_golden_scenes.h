#pragma once

// ============================================================
// GPU 容差 golden 共享场景（tests/support/gpu_golden_scenes.h）
// ------------------------------------------------------------
// 「GPU 容差 golden 层」的场景与帧装配单一来源：同一批场景被 wgpu（`itest_wgpu_golden`）
// 与 GL（`itest_gl_golden`）两条 GPU 实路径共用，各自比对同一张软件 SSOT 基线 PNG。
// 场景绘制体与对应 utest 的 golden 用例逐行同源（改一处必改多处，漂移即软件侧红灯）。
//
// 容差带**不在**本头：GPU↔软件残差随后端管线（WGSL vs GLSL、MSAA vs 软件 AA）而不同，
// 各后端 TU 自行申报常量。帧装配也不申报后端类型——按 `RhiFrameSink` + `read_pixels`
// 的接口形状模板化，`WgpuRhi` 与 `GpuGlRhi` 同形可用；行序差异（GL 帧缓冲自底向上、
// wgpu 离屏读回自顶向下）由调用方以 `bottom_up` 如实申报。
// ============================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/display_list.h"
#include "framework/aurora_test.h"

namespace aurora::testing::gpu_golden {

namespace au = aurora;

// 场景常量：与 `utest_painter_primitives` / `utest_offscreen` / `utest_bar_chart` 同源。
constexpr float HALF_PI = 1.57079632679489661923F;
constexpr float TWO_PI = 6.28318530717958647692F;
constexpr std::uint8_t DIM_ALPHA = 89;

[[nodiscard]] inline auto rect_f(float x, float y, float w, float h) -> au::Rect {
    return au::Rect{.origin = au::Point{.x = x, .y = y}, .size = au::Size{.width = w, .height = h}};
}

// ---- 场景绘制体（几何组：64×64 画布）----

/// @brief 折线场景：直角折线（join）+ 三点细折线 + 半透明折线（顶点不得串珠）。
inline auto draw_polyline(au::Painter &p) -> void {
    constexpr int size = 64;
    p.fill_rect(rect_f(0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size)), au::Color::white());
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 6.0F, .y = 8.0F}, au::Point{.x = 30.0F, .y = 8.0F},
                                             au::Point{.x = 30.0F, .y = 28.0F}},
                      4.0F, au::Color::red());
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 6.0F, .y = 40.0F}, au::Point{.x = 18.0F, .y = 52.0F},
                                             au::Point{.x = 30.0F, .y = 40.0F}, au::Point{.x = 42.0F, .y = 52.0F}},
                      1.5F, au::Color::blue());
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 40.0F, .y = 8.0F}, au::Point{.x = 56.0F, .y = 8.0F},
                                             au::Point{.x = 56.0F, .y = 24.0F}},
                      4.0F, au::Color{255, 0, 0, DIM_ALPHA});
}

/// @brief 扇形场景：实心扇形 + 环扇（donut）+ 整圆 + 弧线描边。
inline auto draw_sector(au::Painter &p) -> void {
    constexpr int size = 64;
    p.fill_rect(rect_f(0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size)), au::Color::white());
    p.fill_sector(au::Point{.x = 16.0F, .y = 16.0F}, 14.0F, 0.0F, -HALF_PI, 0.0F, au::Color::red());
    p.fill_sector(au::Point{.x = 46.0F, .y = 16.0F}, 14.0F, 7.0F, -HALF_PI, HALF_PI, au::Color::blue());
    p.fill_sector(au::Point{.x = 16.0F, .y = 46.0F}, 12.0F, 0.0F, 0.0F, TWO_PI, au::Color::green());
    p.stroke_arc(au::Point{.x = 46.0F, .y = 46.0F}, 10.0F, 3.0F, -HALF_PI, HALF_PI, au::Color{0, 0, 0, 255});
}

// ---- 场景构造体（控件树组：240×120 / 320×200）----

/// @brief 文本列场景（基线 `golden_basic_column`）。
[[nodiscard]] inline auto build_text_column() -> au::Node {
    return au::Node{au::Column{
        au::Text{au::LocalizedString{"Hello, Aurora"}},
        au::Text{au::LocalizedString{"Pixel golden test"}},
    }};
}

/// @brief 柱状图场景（基线 `chart_bar`）：两系列 + 类目轴 + Y 轴刻度 + 圆角条。
[[nodiscard]] inline auto build_bar_chart() -> au::Node {
    au::BarChartProps props{};
    props.series = {au::ChartSeries{.name = "2024", .values = {12.0, 18.0, 9.0, 24.0, 15.0}},
                    au::ChartSeries{.name = "2025", .values = {16.0, 14.0, 20.0, 18.0, 22.0}}};
    props.categories = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    props.axis_y = au::ChartAxisSpec{.label = "sales", .tick_count = 5};
    auto chart = std::make_shared<au::BarChart>(props);
    chart->modifier.set(au::Modifier{}.width(320.0F).height(200.0F));
    return au::Node{au::Column{au::Node{chart}}};
}

// ---- 帧装配：录制 → GPU 重放 → 读回（模板化于 sink 的接口形状）----

/// @brief 读回行序归一：GPU 帧缓冲自底向上（GL）时翻转为 PNG 基线的自顶向下序。
inline auto flip_rows_y(std::vector<std::uint8_t> &px, int w, int h) -> void {
    const auto stride = static_cast<std::size_t>(w) * 4U;
    for (int y = 0; y < h / 2; ++y) {
        const auto top = static_cast<std::size_t>(y) * stride;
        const auto bottom = static_cast<std::size_t>(h - 1 - y) * stride;
        for (std::size_t i = 0; i < stride; ++i) {
            std::swap(px[top + i], px[bottom + i]);
        }
    }
}

/// @brief 命令表版帧：`Painter` 录制 → `begin_frame`（设备像素，dp==px 下 scale=1）→
///        replay → `end_frame` → `read_pixels`。与宿主 GPU 帧路径同构。
/// @tparam Sink 具备 `begin_frame(int,int,float)` / `end_frame()` / `backend()` /
///              `read_pixels(std::vector<std::uint8_t>&)` 的 GPU 后端（`WgpuRhi`、`GpuGlRhi`）
/// @param bottom_up 读回行序自底向上（GL 帧缓冲原序）时为 true，归一到基线的自顶向下序
[[nodiscard]] inline auto render_display_list(auto &sink, int w, int h, const std::function<void(au::Painter &)> &draw,
                                              bool bottom_up) -> au::Image {
    au::DisplayList dl;
    au::Painter p;
    p.begin(w, h);
    p.record(dl);
    draw(p);
    p.stop();
    AURORA_TEST_REQUIRE_TRUE(sink.begin_frame(w, h, 1.0F));
    dl.replay(sink.backend());
    sink.end_frame();
    au::Image img;
    img.width = w;
    img.height = h;
    AURORA_TEST_REQUIRE_TRUE(sink.read_pixels(img.pixels));
    AURORA_TEST_REQUIRE_EQ(img.pixels.size(), static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    if (bottom_up) {
        flip_rows_y(img.pixels, w, h);
    }
    return img;
}

/// @brief 控件树版帧：mount → layout → 录制 paint → GPU 重放读回（同 `render_to_image` 的调度序）。
///        根节点几何落定，供失败报告的控件归因（`collect_widget_boxes`）。
[[nodiscard]] inline auto render_tree(auto &sink, au::Node &root, int w, int h, bool bottom_up) -> au::Image {
    constexpr au::BuildContext ctx;
    root->mount(ctx);
    au::Constraints c;
    c.min = au::Size{.width = 0.0F, .height = 0.0F};
    c.max = au::Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    root->layout(c, ctx);

    au::DisplayList dl;
    au::Painter p;
    p.begin(w, h);
    p.record(dl);
    root->paint(p, rect_f(0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)), ctx);
    p.stop();
    root.set_bounds(rect_f(0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)));

    AURORA_TEST_REQUIRE_TRUE(sink.begin_frame(w, h, 1.0F));
    dl.replay(sink.backend());
    sink.end_frame();
    au::Image img;
    img.width = w;
    img.height = h;
    AURORA_TEST_REQUIRE_TRUE(sink.read_pixels(img.pixels));
    AURORA_TEST_REQUIRE_EQ(img.pixels.size(), static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    if (bottom_up) {
        flip_rows_y(img.pixels, w, h);
    }
    return img;
}

}  // namespace aurora::testing::gpu_golden
