/// 测试类型: integration
/// 目标单元: include/aurora/app/application.h
/// 测试说明: 端到端帧循环性能测试：自定义 Surface + Application.run() 驱动有限帧，验证
///           FrameStats 收集、分阶段计时非负且宽阈值内有界、动画场景帧时间受控、reset 隔离
///           与 HUD 叠加层合成路径。
/// 覆盖说明: 帧时间断言均为「多帧平均 + 宽阈值」（headless 典型值比阈值低 1~2 个数量级），
///           无相对/墙钟计时断言，弱机 CI 稳定。

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/core/log.h"
#include "aurora/window/surface.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_perf_frame_loop {

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

// 构建标准化场景：Column 内放标题 + 20 个按钮，模拟中等复杂度 widget 树。
auto build_scene() -> Node {
    std::vector<Node> children;
    children.reserve(21);
    children.emplace_back(Text{std::string("Performance Benchmark")});
    for (int i = 0; i < 20; ++i) {
        children.emplace_back(Button("Button " + std::to_string(i + 1)));
    }
    return Node{Column{ColumnProps{.children = std::move(children)}}};
}

// 使用 Application + 自定义 Surface 运行 N 帧。
// on_frame 回调可用来强制每帧重绘（绕过脏区 idle 跳过）。
void run_app(int frames, Node view, const std::function<void(Application &)> &on_frame = {}) {
    WindowOptions opts;
    opts.title = "perf_bench";
    opts.size = Size{.width = 800.0F, .height = 600.0F};
    opts.max_frames = frames;
    opts.power_saving = false;  // 基准测原始帧速度：退出事件驱动节流
    Scene scene{std::move(view)};
    Application app{std::move(scene), std::make_unique<MinSurface>(), opts};
    if (on_frame) {
        app.set_on_frame([&app, &on_frame]() -> void { on_frame(app); });
    }
    app.run();
}

}  // namespace

AURORA_TEST_CASE(basic_frame_loop_collects_frame_stats) {
    auto &fs = FrameStats::instance();
    fs.reset();

    // force_full_redraw() 强制每帧都实际渲染（绕过脏区 idle 跳过）。
    run_app(100, build_scene(), [](Application &app) -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });

    // FrameStats 应收集到数据（计数/派生指标，弱机稳定）。
    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "FrameStats total_frames > 0 after run");
    AURORA_TEST_CHECK_MSG(fs.window_size() > 0, "FrameStats window_size > 0 after run");
    AURORA_TEST_CHECK_MSG(fs.avg_frame_ms() > 0.0, "avg_frame_ms > 0");
    AURORA_TEST_CHECK_MSG(fs.fps() > 0.0, "fps > 0");

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

AURORA_TEST_CASE(animated_scene_frame_time_within_budget) {
    auto &fs = FrameStats::instance();
    fs.reset();

    // 含动画场景：on_frame 回调每帧 tick TweenAnimation（结束后重启），
    // 同时 force_full_redraw() 确保每帧实际渲染。
    TweenAnimation anim{0.0F};
    anim.animate_to(1.0F, 0.5);  // 0.5 秒过渡

    run_app(100, build_scene(), [&](Application &app) -> void {
        // 模拟每帧 tick（约 16ms）；动画结束后重新启动以持续驱动。
        anim.tick(0.016);
        if (!anim.is_animating()) {
            const float cur = anim.get();
            anim.animate_to(cur > 0.5F ? 0.0F : 1.0F, 0.5);
        }
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });

    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "animated scene total_frames > 0");
    AURORA_TEST_CHECK_MSG(fs.window_size() > 0, "animated scene window_size > 0");
    // 动画场景帧时间应仍在合理范围（headless 无 vsync，典型 ≪5ms，阈值留 ≥20 倍余量）。
    AURORA_TEST_CHECK_MSG(fs.avg_frame_ms() < 100.0, "animated scene avg_frame_ms < 100ms (wide budget)");

    AURORA_TEST_PRINTF("  total_frames: %zu\n", fs.total_frames());
    AURORA_TEST_PRINTF("  avg_frame_ms: %.3f\n", fs.avg_frame_ms());
    AURORA_TEST_PRINTF("  fps:          %.1f\n", fs.fps());
    AURORA_TEST_PRINTF("  worst_frame:  %.3f ms\n", fs.worst_frame_ms());
    AURORA_TEST_PRINTF("  p99:          %.3f ms\n", fs.percentile_ms(0.99));
}

AURORA_TEST_CASE(phase_timings_nonnegative_and_bounded) {
    auto &fs = FrameStats::instance();
    fs.reset();

    run_app(100, build_scene(), [](Application &app) -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });

    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "phase timings total_frames > 0");

    // 分阶段计时数据应 >= 0（框架调用 record_phases 后会有正值；数值兜底钳非负）。
    AURORA_TEST_CHECK_MSG(fs.avg_layout_ms() >= 0.0, "avg_layout_ms >= 0");
    AURORA_TEST_CHECK_MSG(fs.avg_paint_ms() >= 0.0, "avg_paint_ms >= 0");
    AURORA_TEST_CHECK_MSG(fs.avg_present_ms() >= 0.0, "avg_present_ms >= 0");

    AURORA_TEST_PRINTF("  avg_layout_ms:  %.3f\n", fs.avg_layout_ms());
    AURORA_TEST_PRINTF("  avg_paint_ms:   %.3f\n", fs.avg_paint_ms());
    AURORA_TEST_PRINTF("  avg_present_ms: %.3f\n", fs.avg_present_ms());

    // 有阶段数据（> 0）时验证合理性：headless 纯 CPU 单阶段不应超过 100ms
    // （典型 ≪5ms，阈值留 ≥20 倍余量，吸收弱机单帧调度毛刺）。
    if (fs.avg_paint_ms() > 0.0) {
        AURORA_TEST_CHECK_MSG(fs.avg_paint_ms() < 100.0, "avg_paint_ms < 100ms (wide sanity bound)");
    }
    if (fs.avg_layout_ms() > 0.0) {
        AURORA_TEST_CHECK_MSG(fs.avg_layout_ms() < 100.0, "avg_layout_ms < 100ms (wide sanity bound)");
    }
}

AURORA_TEST_CASE(frame_stats_reset_isolation) {
    // 先运行一轮积累数据。
    auto &fs = FrameStats::instance();
    fs.reset();
    run_app(10, build_scene(), [](Application &app) -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });
    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "accumulated frames before reset");

    const std::size_t before = fs.total_frames();

    // reset 后应清零。
    fs.reset();
    AURORA_TEST_CHECK_EQ(fs.total_frames(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(fs.window_size(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(fs.fps(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(fs.avg_frame_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(fs.worst_frame_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(fs.jitter_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_EQ(fs.dropped_frame_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(fs.hitch_count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(fs.idle_frame_count(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(fs.avg_layout_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(fs.avg_paint_ms(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(fs.avg_present_ms(), 0.0, 1e-9);

    AURORA_TEST_PRINTF("  reset: total_frames %zu -> %zu (isolation verified)\n", before, fs.total_frames());
}

AURORA_TEST_CASE(hud_overlay_composite_path) {
    // 首轮：无叠加层基线，确认仍能正常渲染。
    auto &fs = FrameStats::instance();
    fs.reset();
    run_app(60, build_scene(), [](Application &app) -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
        }
    });
    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "baseline run without overlay renders frames");

    // 次轮：注入 HUD 叠加层（PerfOverlay），验证 present_root 的 composite 路径不崩溃且帧统计正常。
    fs.reset();
    bool installed = false;
    run_app(60, build_scene(), [&](Application &app) -> void {
        if (app.window() != nullptr) {
            app.window()->force_full_redraw();
            if (!installed) {
                app.set_overlay(std::make_shared<PerfOverlay>());
                installed = true;
            }
        }
    });
    AURORA_TEST_CHECK_MSG(installed, "overlay installed via on_frame");
    AURORA_TEST_CHECK_MSG(fs.total_frames() > 0, "overlay run total_frames > 0");
    AURORA_TEST_CHECK_MSG(fs.window_size() > 0, "overlay run window_size > 0");
    AURORA_TEST_CHECK_MSG(fs.avg_frame_ms() > 0.0, "overlay run avg_frame_ms > 0");

    AURORA_TEST_PRINTF("  total_frames: %zu (overlay composited each frame)\n", fs.total_frames());
    AURORA_TEST_PRINTF("  overlay HUD layer path exercised without errors\n");
}

}  // namespace aurora::test_cases::itest_perf_frame_loop
