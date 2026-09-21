/// 测试类型: integration
/// 目标单元: src/aurora/render/gpu/wgpu_rhi.cpp + tests/framework/golden.h
/// 测试说明: GPU 容差 golden 层的 wgpu 侧（确定性双层策略的像素层落地）：同一场景的 DisplayList
///           经离屏 WgpuRhi 重放并 read_pixels 读回，与软件 SSOT 基线 PNG 按场景级申报的
///           单通道容差 + 差异像素预算比对（判据词汇与 golden 红线同源）。场景集覆盖三类跨驱动
///           敏感面：几何 AA（stroke 折线 / 扇形环带）、文本字形（golden_basic_column）、复合控件
///           图表（chart_bar：轴文字 + 圆角条 + 多系列）；场景与帧装配取自
///           tests/support/gpu_golden_scenes.h，GL 侧 itest_gl_golden 共用同一批场景与同一张基线。
///           软件侧零漂移仍由各同名 utest 逐位红线守门，本 TU 只申报 GPU≈软件 的容差带。
///           `AURORA_BACKEND_GPU_WGPU` 未开启或无 wgpu adapter 时按惯例落 skip 桩。

#include <cstddef>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#ifdef AURORA_BACKEND_GPU_WGPU

#include "aurora/render/rhi/wgpu_rhi.h"
#include "framework/golden.h"
#include "support/gpu_golden_scenes.h"

namespace golden = aurora::testing::golden;
namespace scenes = aurora::testing::gpu_golden;

namespace aurora::test_cases::itest_wgpu_golden {
namespace {

// ---- 场景级容差带（Windows Vulkan 与 WSLg vGPU 双侧实测几乎同值——同一 WGSL 管线的确定性
// 残差；预算取实测 ≈2.3× 余量。实测 tol 48 口径：polyline 139px / sector 109px / column 870px /
// chart_bar 5437px；max delta ≤255 来自细笔画错位互补——是定位抖动而非色差）----
constexpr int AURORA_GEOMETRY_TOL = 48;  // 单通道差 > tol 才算差异像素（golden 判据词汇）
constexpr std::size_t AURORA_POLYLINE_BUDGET = 320;  // 64×64 ≈ 8% 画布（描边周长占比较高）
constexpr std::size_t AURORA_SECTOR_BUDGET = 256;  // 64×64 曲线边界同理
constexpr std::size_t AURORA_TEXT_BUDGET = 2048;  // 240×120 字形边缘 AA 与次像素定位抖动
constexpr std::size_t AURORA_CHART_BUDGET = 12800;  // 320×200 网格细线 + 小字号文本复合

/// @brief 每例独立 adapter 装载口径：`WgpuRhi` 禁搬移，须逐例就地构造后查 `valid()`。
[[nodiscard]] auto gpu_options() -> au::rhi::WgpuRhiOptions {
    au::rhi::WgpuRhiOptions opts;
    opts.native_window = nullptr;
    opts.offscreen_width = 1;
    opts.offscreen_height = 1;
    return opts;
}

}  // namespace

#define ITEST_WGPU_GPU_OR_SKIP(gpu)                                                            \
    au::rhi::WgpuRhi gpu(gpu_options());                                                       \
    if (!gpu.valid()) {                                                                        \
        AURORA_TEST_SKIP("无可用 wgpu adapter/device（CI 或驱动缺失），GPU 容差 golden 跳过"); \
    }

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    const auto current = scenes::render_display_list(gpu, 64, 64, scenes::draw_polyline, false);
    golden::compare_gpu_tolerance("painter_polyline", current, AURORA_GEOMETRY_TOL, AURORA_POLYLINE_BUDGET);
}

AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    const auto current = scenes::render_display_list(gpu, 64, 64, scenes::draw_sector, false);
    golden::compare_gpu_tolerance("painter_sector", current, AURORA_GEOMETRY_TOL, AURORA_SECTOR_BUDGET);
}

AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    au::Node root = scenes::build_text_column();
    const auto current = scenes::render_tree(gpu, root, 240, 120, false);
    golden::compare_gpu_tolerance("golden_basic_column", current, AURORA_GEOMETRY_TOL, AURORA_TEXT_BUDGET, &root);
}

AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    ITEST_WGPU_GPU_OR_SKIP(gpu);
    au::Node root = scenes::build_bar_chart();
    const auto current = scenes::render_tree(gpu, root, 320, 200, false);
    golden::compare_gpu_tolerance("chart_bar", current, AURORA_GEOMETRY_TOL, AURORA_CHART_BUDGET, &root);
}

}  // namespace aurora::test_cases::itest_wgpu_golden

#else  // 后端未编译

namespace aurora::test_cases::itest_wgpu_golden {

AURORA_TEST_CASE(gpu_polyline_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启，WgpuRhi 离屏通路整体被宏剔除");
}
AURORA_TEST_CASE(gpu_sector_within_tolerance_of_software_golden) { AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启"); }
AURORA_TEST_CASE(gpu_text_column_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}
AURORA_TEST_CASE(gpu_bar_chart_within_tolerance_of_software_golden) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 未开启");
}

}  // namespace aurora::test_cases::itest_wgpu_golden

#endif  // AURORA_BACKEND_GPU_WGPU
