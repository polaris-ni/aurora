/// 测试类型: integration
/// 目标单元: src/aurora/render/gpu/wgpu_rhi.cpp + tests/framework/golden.h
/// 测试说明: GPU 容差 golden 层（确定性双层策略的像素层落地）：同一场景的 DisplayList 经离屏
///           WgpuRhi 重放并 read_pixels 读回，与软件 SSOT 基线 PNG 按场景级申报的
///           单通道容差 + 差异像素预算比对（判据词汇与 golden 红线同源）。场景集覆盖三类
///           跨驱动敏感面：几何 AA（stroke 折线 / 扇形环带）、文本字形（golden_basic_column）、
///           复合控件图表（chart_bar：轴文字 + 圆角条 + 多系列）。基线与同名 utest 共用同一张
///           PNG——软件侧零漂移仍由各 utest 逐位红线守门，本 TU 只申报 GPU≈软件 的容差带。
///           `AURORA_BACKEND_GPU_WGPU` 未开启或无 wgpu adapter 时按惯例落 skip 桩。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#ifdef AURORA_BACKEND_GPU_WGPU

#include "aurora/aurora.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/wgpu_rhi.h"
#include "framework/golden.h"

namespace golden = aurora::testing::golden;

namespace aurora::test_cases::itest_wgpu_golden {
namespace {

constexpr float HALF_PI = 1.57079632679489661923F;
constexpr float TWO_PI = 6.28318530717958647692F;
constexpr std::uint8_t DIM_ALPHA = 89;

// ---- 场景级容差带（Windows Vulkan 与 WSLg vGPU 双侧实测几乎同值——同一 WGSL 管线的确定性
// 残差；预算取实测 ≈2.3× 余量。实测 tol 48 口径：polyline 139px / sector 109px / column 870px /
// chart_bar 5437px；max delta ≤255 来自细笔画错位互补——是定位抖动而非色差）----
constexpr int kGeometryTol = 48;                  // 单通道差 > tol 才算差异像素（golden 判据词汇）
constexpr std::size_t kPolylineBudget = 320;      // 64×64 ≈ 8% 画布（描边周长占比较高）
constexpr std::size_t kSectorBudget = 256;        // 64×64 曲线边界同理
constexpr std::size_t kTextBudget = 2048;         // 240×120 字形边缘 AA 与次像素定位抖动
constexpr std::size_t kChartBudget = 12800;       // 320×200 网格细线 + 小字号文本复合

[[nodiscard]] auto rect_f(float x, float y, float w, float h) -> au::Rect {
    return au::Rect{.origin = au::Point{.x = x, .y = y}, .size = au::Size{.width = w, .height = h}};
}

/// @brief 录制 → GPU 重放 → 读回：与宿主帧路径同构（begin_frame 收设备像素，dp==px 下 scale=1）。
[[nodiscard]] auto gpu_render(au::rhi::WgpuRhi &gpu, int w, int h,
                              const std::function<void(au::Painter &)> &draw) -> au::Image {
    au::DisplayList dl;
    au::Painter p;
    p.begin(w, h);
    p.record(dl);
    draw(p);
    p.stop();
    AURORA_TEST_REQUIRE_TRUE(gpu.begin_frame(w, h, 1.0F));
    dl.replay(gpu.backend());
    gpu.end_frame();
    au::Image img;
    img.width = w;
    img.height = h;
    AURORA_TEST_REQUIRE_TRUE(gpu.read_pixels(img.pixels));
    AURORA_TEST_REQUIRE_EQ(img.pixels.size(), static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    return img;
}

/// @brief 控件树版：mount → layout → 录制 paint → GPU 重放读回（同 `render_to_image` 的调度序）。
[[nodiscard]] auto gpu_render_tree(au::rhi::WgpuRhi &gpu, au::Node &root, int w, int h) -> au::Image {
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
    // 几何落定到根：失败报告的控件归因（collect_widget_boxes）依赖已定布局的树。
    root.set_bounds(rect_f(0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)));

    AURORA_TEST_REQUIRE_TRUE(gpu.begin_frame(w, h, 1.0F));
    dl.replay(gpu.backend());
    gpu.end_frame();
    au::Image img;
    img.width = w;
    img.height = h;
    AURORA_TEST_REQUIRE_TRUE(gpu.read_pixels(img.pixels));
    AURORA_TEST_REQUIRE_EQ(img.pixels.size(), static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    return img;
}

// ---- 场景绘制体：与对应 utest golden 用例逐行同源（改一处必改两处，漂移即红灯）----

auto draw_polyline(au::Painter &p) -> void {
    constexpr int size = 64;
    p.fill_rect(rect_f(0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size)), au::Color::white());
    // 直角折线（join）+ 三点折线（细）+ 半透明折线（D13：顶点不得串珠）
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 6.0F, .y = 8.0F},
                                             au::Point{.x = 30.0F, .y = 8.0F},
                                             au::Point{.x = 30.0F, .y = 28.0F}},
                      4.0F, au::Color::red());
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 6.0F, .y = 40.0F},
                                             au::Point{.x = 18.0F, .y = 52.0F},
                                             au::Point{.x = 30.0F, .y = 40.0F},
                                             au::Point{.x = 42.0F, .y = 52.0F}},
                      1.5F, au::Color::blue());
    p.stroke_polyline(std::vector<au::Point>{au::Point{.x = 40.0F, .y = 8.0F},
                                             au::Point{.x = 56.0F, .y = 8.0F},
                                             au::Point{.x = 56.0F, .y = 24.0F}},
                      4.0F, au::Color{255, 0, 0, DIM_ALPHA});
}

auto draw_sector(au::Painter &p) -> void {
    constexpr int size = 64;
    p.fill_rect(rect_f(0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size)), au::Color::white());
    // 实心扇形（12 点起，90°）+ 环扇（donut）+ 整圆 + 弧线描边
    p.fill_sector(au::Point{.x = 16.0F, .y = 16.0F}, 14.0F, 0.0F, -HALF_PI, 0.0F, au::Color::red());
    p.fill_sector(au::Point{.x = 46.0F, .y = 16.0F}, 14.0F, 7.0F, -HALF_PI, HALF_PI, au::Color::blue());
    p.fill_sector(au::Point{.x = 16.0F, .y = 46.0F}, 12.0F, 0.0F, 0.0F, TWO_PI, au::Color::green());
    p.stroke_arc(au::Point{.x = 46.0F, .y = 46.0F}, 10.0F, 3.0F, -HALF_PI, HALF_PI, au::Color{0, 0, 0, 255});
}

[[nodiscard]] auto build_text_column() -> au::Node {
    return au::Node{au::Column{
        au::Text{au::LocalizedString{"Hello, Aurora"}},
        au::Text{au::LocalizedString{"Pixel golden test"}},
    }};
}

[[nodiscard]] auto build_bar_chart() -> au::Node {
    au::BarChartProps props{};
    props.series = {au::ChartSeries{.name = "2024", .values = {12.0, 18.0, 9.0, 24.0, 15.0}},
                    au::ChartSeries{.name = "2025", .values = {16.0, 14.0, 20.0, 18.0, 22.0}}};
    props.categories = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    props.axis_y = au::ChartAxisSpec{.label = "sales", .tick_count = 5};
    auto chart = std::make_shared<au::BarChart>(props);
    chart->modifier.set(au::Modifier{}.width(320.0F).height(200.0F));
    return au::Node{au::Column{au::Node{chart}}};
}

/// @brief 每例独立 adapter 装载口径：`WgpuRhi` 禁搬移，须逐例就地构造后查 `valid()`。
[[nodiscard]] auto gpu_options() -> au::rhi::WgpuRhiOptions {
    au::rhi::WgpuRhiOptions opts;
    opts.native_window = nullptr;
    opts.offscreen_width = 1;
    opts.offscreen_height = 1;
    return opts;
}

}  // namespace

#define ITEST_WGPU_GPU_OR_SKIP(gpu)                                                                            \
    au::rhi::WgpuRhi gpu(gpu_options());                                                                       \
    if (!gpu.valid()) {                                                                                        \
        AURORA_TEST_SKIP("无可用 wgpu adapter/device（CI 或驱动缺失），GPU 容差 golden 跳过");                  \
    }

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    const auto current = gpu_render(gpu, 64, 64, [](au::Painter &p) { draw_polyline(p); });
    golden::compare_gpu_tolerance("painter_polyline", current, kGeometryTol, kPolylineBudget);
}

AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    const auto current = gpu_render(gpu, 64, 64, [](au::Painter &p) { draw_sector(p); });
    golden::compare_gpu_tolerance("painter_sector", current, kGeometryTol, kSectorBudget);
}

AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    au::Node root = build_text_column();
    const auto current = gpu_render_tree(gpu, root, 240, 120);
    golden::compare_gpu_tolerance("golden_basic_column", current, kGeometryTol, kTextBudget, &root);
}

AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    au::Node root = build_bar_chart();
    const auto current = gpu_render_tree(gpu, root, 320, 200);
    golden::compare_gpu_tolerance("chart_bar", current, kGeometryTol, kChartBudget, &root);
}

}  // namespace aurora::test_cases::itest_wgpu_golden

#else  // 后端未编译

namespace aurora::test_cases::itest_wgpu_golden {

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启，WgpuRhi 离屏通路整体被宏剔除");
}
AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}

}  // namespace aurora::test_cases::itest_wgpu_golden

#endif  // AURORA_BACKEND_GPU_WGPU
