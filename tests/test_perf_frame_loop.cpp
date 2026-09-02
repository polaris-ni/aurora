// test_perf_frame_loop.cpp — 端到端帧循环性能基准测试。
// 使用自定义 Surface + Application.run() 模式运行 100 帧，
// 验证 FrameStats 收集数据、分阶段计时合理、动画场景帧时间在阈值内。
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
using aurora::LocalizedString;
using aurora::Node;
using aurora::Painter;
using aurora::PerfOverlay;
using aurora::Result;
using aurora::Scene;
using aurora::Size;
using aurora::Surface;
using aurora::Text;
using aurora::TextProps;
using aurora::TweenAnimation;
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

// 构建标准化场景：Column 内放标题 + 20 个按钮，模拟中等复杂度 widget 树。
auto build_scene() -> Node {
    std::vector<Node> children;
    children.reserve(21);
    children.emplace_back(Text{ TextProps{ .content = LocalizedString{ "Performance Benchmark" } } });
    for (int i = 0; i < 20; ++i) {
        char label[32];
        std::snprintf(label, sizeof(label), "Button %d", i + 1);
        children.emplace_back(Button(std::string(label)));
    }
    return Node{ Column{ ColumnProps{ .children = std::move(children) } } };
}

// 使用 Application + 自定义 Surface 运行 N 帧。
// on_frame 回调可用来强制每帧重绘（绕过脏区跳过）。
void run_app(int frames, Node view, const std::function<void(Application &)> &on_frame = {}) {
    WindowOptions opts;
    opts.title = "perf_bench";
    opts.size = Size{ .width = 800.0f, .height = 600.0f };
    opts.max_frames = frames;
    opts.power_saving = false; // 基准测原始帧速度：退出事件驱动节流
    Scene scene{ std::move(view) };
    Application app{ std::move(scene), std::make_unique<MinSurface>(), opts };
    if (on_frame) {
        app.set_on_frame([&]() -> void { on_frame(app); });
    }
    app.run();
}

// ============================================================
// 测试 1：基本帧循环 + FrameStats 集成
// ============================================================
void test_basic_frame_loop() {
    AURORA_TEST_PRINTF("\n=== Test 1: Basic frame loop + FrameStats ===\n");
    FrameStats::instance().reset();

    // 通过 force_full_redraw() 强制每帧都实际渲染（绕过脏区 idle 跳过）
    run_app(100, build_scene(), [](const Application &app) -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });

    auto const &fs = FrameStats::instance();
    // FrameStats 应收集到数据
    AURORA_TEST_CHECK(fs.total_frames() > 0);
    AURORA_TEST_CHECK(fs.window_size() > 0);
    AURORA_TEST_CHECK(fs.avg_frame_ms() > 0.0);
    AURORA_TEST_CHECK(fs.fps() > 0.0);

    // 打印性能摘要
    AURORA_TEST_PRINTF("  total_frames: %zu\n", fs.total_frames());
    AURORA_TEST_PRINTF("  window_size:  %zu\n", fs.window_size());
    AURORA_TEST_PRINTF("  avg_frame_ms: %.3f\n", fs.avg_frame_ms());
    AURORA_TEST_PRINTF("  fps:          %.1f\n", fs.fps());
    AURORA_TEST_PRINTF("  worst_frame:  %.3f ms\n", fs.worst_frame_ms());
    AURORA_TEST_PRINTF("  jitter_ms:    %.3f\n", fs.jitter_ms());
    AURORA_TEST_PRINTF("  p99:          %.3f ms\n", fs.percentile_ms(0.99));
    AURORA_TEST_PRINTF("  dropped:      %zu\n", fs.dropped_frame_count());
    AURORA_TEST_PRINTF("  hitch:        %zu\n", fs.hitch_count());
}

// ============================================================
// 测试 2：含动画的场景
// ============================================================
void test_animated_scene() {
    AURORA_TEST_PRINTF("\n=== Test 2: Animated scene ===\n");
    FrameStats::instance().reset();

    // 构建含动画的场景：on_frame 回调驱动 TweenAnimation 每帧 tick，
    // 同时 force_full_redraw() 确保每帧实际渲染。
    TweenAnimation anim{ 0.0f };
    anim.animate_to(1.0f, 0.5); // 0.5 秒过渡

    run_app(100, build_scene(), [&](const Application &app) -> void {
        // 模拟每帧 tick（约 16ms）；动画结束后重新启动以持续驱动。
        anim.tick(0.016);
        if (!anim.is_animating()) {
            anim.animate_to(anim.get() > 0.5f ? 0.0f : 1.0f, 0.5);
        }
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });

    auto const &fs = FrameStats::instance();
    AURORA_TEST_CHECK(fs.total_frames() > 0);
    AURORA_TEST_CHECK(fs.window_size() > 0);
    // 动画场景帧时间应仍在合理范围（headless 无 vsync，宽松阈值 100ms）
    AURORA_TEST_CHECK(fs.avg_frame_ms() < 100.0);

    AURORA_TEST_PRINTF("  total_frames: %zu\n", fs.total_frames());
    AURORA_TEST_PRINTF("  avg_frame_ms: %.3f\n", fs.avg_frame_ms());
    AURORA_TEST_PRINTF("  fps:          %.1f\n", fs.fps());
    AURORA_TEST_PRINTF("  worst_frame:  %.3f ms\n", fs.worst_frame_ms());
    AURORA_TEST_PRINTF("  p99:          %.3f ms\n", fs.percentile_ms(0.99));
}

// ============================================================
// 测试 3：分阶段计时验证
// ============================================================
void test_phase_timings() {
    AURORA_TEST_PRINTF("\n=== Test 3: Phase timings ===\n");
    FrameStats::instance().reset();

    run_app(100, build_scene(), [](const Application &app) -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });

    auto const &fs = FrameStats::instance();
    AURORA_TEST_CHECK(fs.total_frames() > 0);

    // 分阶段计时数据应 >= 0（框架调用 record_phases 后会有正值）
    AURORA_TEST_CHECK(fs.avg_layout_ms() >= 0.0);
    AURORA_TEST_CHECK(fs.avg_paint_ms() >= 0.0);
    AURORA_TEST_CHECK(fs.avg_present_ms() >= 0.0);

    AURORA_TEST_PRINTF("  avg_layout_ms:  %.3f\n", fs.avg_layout_ms());
    AURORA_TEST_PRINTF("  avg_paint_ms:   %.3f\n", fs.avg_paint_ms());
    AURORA_TEST_PRINTF("  avg_present_ms: %.3f\n", fs.avg_present_ms());

    // 如果有阶段数据（> 0），验证合理性：paint 不应超过 50ms（headless 纯 CPU）
    if (fs.avg_paint_ms() > 0.0) {
        AURORA_TEST_CHECK(fs.avg_paint_ms() < 50.0);
    }
    if (fs.avg_layout_ms() > 0.0) {
        AURORA_TEST_CHECK(fs.avg_layout_ms() < 50.0);
    }
}

// ============================================================
// 测试 4：FrameStats reset 隔离性
// ============================================================
void test_stats_isolation() {
    AURORA_TEST_PRINTF("\n=== Test 4: FrameStats reset isolation ===\n");

    // 先运行一轮积累数据
    FrameStats::instance().reset();
    run_app(10, build_scene(), [](const Application &app) -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });
    auto &fs = FrameStats::instance();
    AURORA_TEST_CHECK(fs.total_frames() > 0);

    const std::size_t before = fs.total_frames();

    // reset 后应清零
    fs.reset();
    AURORA_TEST_CHECK(fs.total_frames() == 0);
    AURORA_TEST_CHECK(fs.window_size() == 0);
    AURORA_TEST_CHECK(fs.fps() == 0.0);
    AURORA_TEST_CHECK(fs.avg_frame_ms() == 0.0);
    AURORA_TEST_CHECK(fs.worst_frame_ms() == 0.0);
    AURORA_TEST_CHECK(fs.jitter_ms() == 0.0);
    AURORA_TEST_CHECK(fs.dropped_frame_count() == 0);
    AURORA_TEST_CHECK(fs.hitch_count() == 0);
    AURORA_TEST_CHECK(fs.idle_frame_count() == 0);
    AURORA_TEST_CHECK(fs.avg_layout_ms() == 0.0);
    AURORA_TEST_CHECK(fs.avg_paint_ms() == 0.0);
    AURORA_TEST_CHECK(fs.avg_present_ms() == 0.0);

    AURORA_TEST_PRINTF("  reset before total_frames: %zu -> after reset: %zu\n", before, fs.total_frames());
    AURORA_TEST_PRINTF("  isolation verified\n");
}

// ============================================================
// 测试 5：HUD 叠加层（分层 HUD / set_overlay）
// ============================================================
void test_overlay_hud() {
    AURORA_TEST_PRINTF("\n=== Test 5: HUD overlay layer (set_overlay) ===\n");
    FrameStats::instance().reset();

    // 首轮：无叠加层基线，确认仍能正常渲染。
    run_app(60, build_scene(), [](const Application &app) -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
        }
    });
    auto const &fs0 = FrameStats::instance();
    AURORA_TEST_CHECK(fs0.total_frames() > 0);

    // 次轮：注入 HUD 叠加层（PerfOverlay），验证 present_root 的 composite 路径不崩溃且帧统计正常。
    FrameStats::instance().reset();
    bool installed = false;
    run_app(60, build_scene(), [&](const Application &app) -> void {
        if (app.window()) {
            app.window()->force_full_redraw();
            if (!installed) {
                app.window()->set_overlay(std::make_shared<PerfOverlay>());
                installed = true;
            }
        }
    });
    auto const &fs = FrameStats::instance();
    AURORA_TEST_CHECK(fs.total_frames() > 0);
    AURORA_TEST_CHECK(fs.window_size() > 0);
    AURORA_TEST_CHECK(fs.avg_frame_ms() > 0.0);

    AURORA_TEST_PRINTF("  total_frames: %zu (overlay composited each frame)\n", fs.total_frames());
    AURORA_TEST_PRINTF("  overlay HUD layer path exercised without errors\n");
}

} // namespace

AURORA_TEST() {
    test_basic_frame_loop();
    test_animated_scene();
    test_phase_timings();
    test_stats_isolation();
    test_overlay_hud();
}
