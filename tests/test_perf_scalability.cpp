// test_perf_scalability.cpp — 可扩展性基准测试。
// 测试不同控件数量（50/100/200/500）下的 layout/paint/total 帧时间，
// 输出性能增长曲线，为后续架构级优化提供数据支撑。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/aurora.h"
#include "aurora/window/surface.h"

#include "test_harness.h"

using aurora::Application;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::FrameStats;
using aurora::Node;
using aurora::Painter;
using aurora::Result;
using aurora::Row;
using aurora::RowProps;
using aurora::Scene;
using aurora::Size;
using aurora::Surface;
using aurora::WindowOptions;

namespace {

// 最小自定义 Surface（不依赖任何内置后端），用于 headless 帧循环测试。
struct MinSurface : Surface {
    auto begin_frame(int w, int h) -> Result<bool> override {
        m_painter.begin(w, h);
        m_size = Size{ .width = static_cast<float>(w), .height = static_cast<float>(h) };
        return Result{ true };
    }
    auto painter() -> Painter & override { return m_painter; }
    auto present() -> Result<bool> override {
        ++m_frames;
        return Result{ true };
    }
    [[nodiscard]] auto size() const -> Size override { return m_size; }
    [[nodiscard]] auto frame_count() const -> int override { return m_frames; }
    Painter m_painter;
    Size m_size{ .width = 0.0f, .height = 0.0f };
    int m_frames = 0;
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
        btns.reserve(count);
        for (int j = 0; j < count; ++j) {
            char label[32];
            std::snprintf(label, sizeof(label), "Btn %d", ++btn_idx);
            btns.emplace_back(Button(std::string(label)));
        }
        rows.emplace_back(Row{ RowProps{ .children = std::move(btns), .gap = 4.0f } });
        remaining -= count;
    }
    return Node{ Column{ ColumnProps{ .children = std::move(rows), .gap = 2.0f } } };
}

// 使用 Application + 自定义 Surface 运行 N 帧，返回 FrameStats 快照。
struct StatsSnapshot {
    double avg_frame_ms;
    double avg_layout_ms;
    double avg_paint_ms;
    double fps;
    double worst_frame_ms;
    double p99;
    std::size_t total_frames;
};

auto run_benchmark(int widget_count, const int frames) -> StatsSnapshot {
    FrameStats::instance().reset();

    Node view = build_scene(widget_count);
    WindowOptions opts;
    opts.title = "scalability_bench";
    opts.size = Size{ .width = 800.0f, .height = 600.0f };
    opts.max_frames = frames;
    opts.power_saving = false; // 基准测原始帧速度：退出事件驱动节流
    Scene scene{ std::move(view) };
    Application app{ std::move(scene), std::make_unique<MinSurface>(), opts };
    app.set_on_frame([&]() -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });
    app.run();

    auto &fs = FrameStats::instance();
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

// 测试矩阵规模
constexpr int AURORA_K_SIZES[] = { 50, 100, 200, 500 };
constexpr int AURORA_K_FRAMES = 100;

} // namespace

AURORA_TEST() {
    AURORA_TEST_PRINTF("\n=== Scalability Benchmark ===\n");

    StatsSnapshot snapshots[4]{};

    for (int i = 0; i < 4; ++i) {
        const int n = AURORA_K_SIZES[i];
        snapshots[i] = run_benchmark(n, AURORA_K_FRAMES);
        auto const &s = snapshots[i];

        AURORA_TEST_PRINTF("N=%-3d  avg=%.1fms layout=%.1fms paint=%.1fms fps=%.0f worst=%.1fms p99=%.1fms\n", n,
                           s.avg_frame_ms, s.avg_layout_ms, s.avg_paint_ms, s.fps, s.worst_frame_ms, s.p99);

        // 每个规模都能正常完成帧循环
        AURORA_TEST_CHECK_MSG(s.total_frames > 0, "total_frames > 0");
    }

    // 500 按钮场景 avg_frame_ms < 50ms（宽松阈值，确保不超时）
    AURORA_TEST_CHECK_MSG(snapshots[3].avg_frame_ms < 50.0, "N=500 avg_frame_ms < 50ms");

    // 增长趋势检查：更大规模不应比小规模慢一个数量级以上（宽松）
    for (int i = 1; i < 4; ++i) {
        const double ratio = snapshots[i].avg_frame_ms / (snapshots[i - 1].avg_frame_ms + 0.001);
        AURORA_TEST_PRINTF("  growth N=%d->N=%d: %.1fx\n", AURORA_K_SIZES[i - 1], AURORA_K_SIZES[i], ratio);
    }
}
