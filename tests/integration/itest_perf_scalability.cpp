/// 测试类型: integration
/// 目标单元: include/aurora/app/application.h
/// 测试说明: 可扩展性基准：控件规模 50/100/200/500 经完整帧循环（Application + 自定义 Surface +
///           FrameStats），断言各规模均能完成帧循环（计数断言）；最大规模帧时间经 3 次独立采样取
///           中位数后落在宽阈值内（无性能爆炸）；规模间增长倍率仅打印观测，不作时序敏感断言。
/// 覆盖说明: 计时断言 = 中位数(3 次) × 100ms 宽阈值（headless 典型值 ≪10ms，余量 ≥1 个数量级）。

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/core/log.h"
#include "aurora/window/surface.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_perf_scalability {

namespace {

// 最小自定义 Surface（不依赖任何内置后端），用于 headless 帧循环测试。
class MinSurface final : public Surface {
  public:
    auto begin_frame(int w, int h) -> Result<bool> override {
        painter_.begin(w, h);
        size_ = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
        return Result<bool>{true};
    }
    auto painter() -> Painter & override { return painter_; }
    auto present() -> Result<bool> override {
        ++frames_;
        return Result<bool>{true};
    }
    [[nodiscard]] auto size() const -> Size override { return size_; }
    [[nodiscard]] auto frame_count() const -> int override { return frames_; }

  private:
    Painter painter_;
    Size size_{.width = 0.0F, .height = 0.0F};
    int frames_ = 0;
};

// 构建嵌套 Column > Row > Button 场景，模拟更真实的布局压力。
// 每行放 3 个 Button，共 ceil(N/3) 行，总计 N 个 Button。
auto build_scene(int n) -> Node {
    constexpr int k_buttons_per_row = 3;
    std::vector<Node> rows;
    int remaining = n;
    int btn_idx = 0;
    while (remaining > 0) {
        const int count = (remaining >= k_buttons_per_row) ? k_buttons_per_row : remaining;
        std::vector<Node> btns;
        btns.reserve(static_cast<std::size_t>(count));
        for (int j = 0; j < count; ++j) {
            ++btn_idx;
            btns.emplace_back(Button("Btn " + std::to_string(btn_idx)));
        }
        rows.emplace_back(Row{RowProps{.children = std::move(btns), .gap = 4.0F}});
        remaining -= count;
    }
    return Node{Column{ColumnProps{.children = std::move(rows), .gap = 2.0F}}};
}

// FrameStats 快照（run 结束后一次性读取）。
struct StatsSnapshot {
    double avg_frame_ms = 0.0;
    double avg_layout_ms = 0.0;
    double avg_paint_ms = 0.0;
    double fps = 0.0;
    double worst_frame_ms = 0.0;
    double p99 = 0.0;
    std::size_t total_frames = 0;
};

// 使用 Application + 自定义 Surface 运行 frames 帧，返回 FrameStats 快照。
auto run_benchmark(int widget_count, int frames) -> StatsSnapshot {
    FrameStats::instance().reset();

    WindowOptions opts;
    opts.title = "scalability_bench";
    opts.size = Size{.width = 800.0F, .height = 600.0F};
    opts.max_frames = frames;
    opts.power_saving = false;  // 基准测原始帧速度：退出事件驱动节流
    Scene scene{build_scene(widget_count)};
    Application app{std::move(scene), std::make_unique<MinSurface>(), opts};
    app.set_on_frame([&app]() -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });
    app.run();

    const FrameStats &fs = FrameStats::instance();
    return StatsSnapshot{
        .avg_frame_ms = fs.avg_frame_ms(),
        .avg_layout_ms = fs.avg_layout_ms(),
        .avg_paint_ms = fs.avg_paint_ms(),
        .fps = fs.fps(),
        .worst_frame_ms = fs.worst_frame_ms(),
        .p99 = fs.percentile_ms(0.99),
        .total_frames = fs.total_frames(),
    };
}

constexpr int k_sizes[] = {50, 100, 200, 500};
constexpr int k_matrix_frames = 100;

}  // namespace

AURORA_TEST_CASE(benchmark_matrix_completes_all_sizes) {
    StatsSnapshot snapshots[std::size(k_sizes)]{};

    for (std::size_t i = 0; i < std::size(k_sizes); ++i) {
        const int n = k_sizes[i];
        snapshots[i] = run_benchmark(n, k_matrix_frames);
        const StatsSnapshot &s = snapshots[i];

        AURORA_TEST_PRINTF("N=%-3d  avg=%.1fms layout=%.1fms paint=%.1fms fps=%.0f worst=%.1fms p99=%.1fms\n", n,
                           s.avg_frame_ms, s.avg_layout_ms, s.avg_paint_ms, s.fps, s.worst_frame_ms, s.p99);

        // 每个规模都能正常完成帧循环（纯计数断言，弱机稳定）。
        AURORA_TEST_CHECK_MSG(s.total_frames > 0, "total_frames > 0 for all sizes");
    }

    // 增长趋势观测：更大规模不应比小规模慢一个数量级以上（仅打印，供架构级优化参考，
    // 不作时序敏感断言）。
    for (std::size_t i = 1; i < std::size(k_sizes); ++i) {
        const double ratio = snapshots[i].avg_frame_ms / (snapshots[i - 1].avg_frame_ms + 0.001);
        AURORA_TEST_PRINTF("  growth N=%d->N=%d: %.1fx\n", k_sizes[i - 1], k_sizes[i], ratio);
    }
}

AURORA_TEST_CASE(largest_scene_frame_time_median_within_budget) {
    // 最大规模（N=500）独立采样 3 次，取中位数——单次运行遇弱机噪声毛刺仍稳定。
    constexpr int k_n = 500;
    constexpr int k_frames = 60;
    double samples[3] = {0.0, 0.0, 0.0};
    for (double &ms : samples) {
        ms = run_benchmark(k_n, k_frames).avg_frame_ms;
    }
    std::sort(samples, samples + 3);
    const double median = samples[1];
    AURORA_TEST_PRINTF("  N=%d avg_frame_ms samples: %.1f / %.1f / %.1f -> median %.1fms\n", k_n, samples[0],
                       samples[1], samples[2], median);

    // 宽阈值：headless 纯 CPU 典型 ≪10ms；100ms 阈值留 ≥1 个数量级余量，
    // 仅拦截「性能爆炸」级回归（旧阈值 50ms 的 2 倍宽松度 + 中位数采样）。
    AURORA_TEST_CHECK_MSG(median < 100.0, "N=500 median avg_frame_ms < 100ms (no performance explosion)");
}

}  // namespace aurora::test_cases::itest_perf_scalability
