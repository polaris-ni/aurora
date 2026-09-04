// Google Play 风格桌面应用商店（Material / Aurora 原生实现）。
//
// 数据层完全本地合成（确定性目录 + 程序化 Image），经 DataHook 抽象，绝不联网。
// 缺失的基础组件（横向虚拟列表 LazyRow、底部导航栏 BottomNavBar）已补全进框架；
// 并修复了 GridView 在圆角裁剪容器内越界崩溃的已知 BUG。
#include <chrono>
#include <memory>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/debug/debug_paint.h"
#include "aurora/debug/debug_runtime.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#ifdef AURORA_INSPECTOR_SERVER_ENABLED
#include "aurora/inspector/inspector_server.h"
#endif
#include "google_play_data.h"
#include "google_play_ui.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto &repo = gp::repository();
    aurora::Animator anim;
    au::Reactive dark{false};

    auto host = std::make_shared<aurora::NavigatorHost>(anim);
    auto on_open = [&repo, host, &dark](const std::string &id) -> void {
        gp::ui::push_detail_route(host, &repo, id, [host]() -> void { (void)host->pop(); }, &dark);
    };

    host->push(au::Route{au::Node{std::make_shared<gp::ui::AppShell>(&repo, on_open, &dark)}, "home", {}});

    // 帧率可视化：用框架内置 PerfOverlay 作为独立 HUD 叠加层（分层 HUD），
    // 经 Application::set_overlay 注入；它独立于 widget 树以 ~2Hz 重绘自身、不再触发整树重绘。
    // FrameStats 已由 Application::run 每帧自动 record(dt)，PerfOverlay 直接消费。
    aurora::Scene scene{au::Node{host}};
    aurora::WindowOptions opts;
    opts.max_fps = 0;  // 解除帧率上限：内部帧循环以显示器/GPU 允许的最高速率运行，滚动与动画更跟手
    opts.size = au::Size{.width = 1100.0F, .height = 760.0F};
    opts.title = "Google Play";
    auto win_res = create_native_window(opts);
    aurora::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    app.set_overlay(std::make_shared<au::PerfOverlay>());
    auto last_fps_print = std::chrono::steady_clock::now();
    app.set_on_frame([&anim, &last_fps_print]() -> void {
        anim.tick(1.0 / 60.0);
        // 每秒向 stdout 打印一次帧率摘要（节流，避免刷屏）。FrameStats 由 Application::run 每帧填充。
        const auto now = std::chrono::steady_clock::now();
        const double since_s = std::chrono::duration<double>(now - last_fps_print).count();
        if (since_s >= 1.0) {
            last_fps_print = now;
            const auto &s = au::FrameStats::instance();
            AURORA_LOG_RAW("demo", "FPS ", s.fps(), " | avg ", s.avg_frame_ms(), "ms | P99 ", s.percentile_ms(0.99),
                           "ms | jitter ", s.jitter_ms(), "ms | dropped ", s.dropped_frame_count(), " | layout ",
                           s.avg_layout_ms(), "ms | paint ", s.avg_paint_ms(), "ms | present ", s.avg_present_ms(),
                           "ms\n");
        }
    });
    // ---- DEBUG 能力接入（DEBUG_BACKEND 演示）----
    // 全部编译宏门控：本地快捷键仅在 AURORA_ENABLE_DEBUG（Debug/RelWithDebInfo）生效；
    // InspectorServer 仅当链接 aurora_inspector_server 库（构建带 -DAURORA_BUILD_INSPECTOR_SERVER=ON）时生效。
    // Release 构建下这些分支整体编译剔除，零运行时开销、零链接依赖。
#ifdef AURORA_ENABLE_DEBUG
    {
        // 可视化调试叠层开关（全局）；set_flags 后 Window::present_root 在下一帧自动绘制叠层。
        au::debug::set_output_directory("aurora_debug");
        au::debug::DebugPaintFlags dbg_flags{};
        auto toggle_flag = [&dbg_flags](bool &field, const char *name) -> void {
            field = !field;
            au::debug::set_flags(dbg_flags);
            AURORA_LOG_RAW("demo", "debug overlay ", name, ": ", field ? "ON" : "OFF", "\n");
        };
        app.shortcuts().add(au::KeyCombo{au::KeyCode::F1},
                            [&]() -> void { toggle_flag(dbg_flags.layout_guides, "layout_guides"); });
        app.shortcuts().add(au::KeyCombo{au::KeyCode::F2},
                            [&]() -> void { toggle_flag(dbg_flags.relayout_boundaries, "relayout_boundaries"); });
        app.shortcuts().add(au::KeyCombo{au::KeyCode::F3},
                            [&]() -> void { toggle_flag(dbg_flags.layer_borders, "layer_borders"); });
        app.shortcuts().add(au::KeyCombo{au::KeyCode::F4},
                            [&]() -> void { toggle_flag(dbg_flags.repaint_highlight, "repaint_highlight"); });
        app.shortcuts().add(au::KeyCombo{au::KeyCode::F5},
                            [&]() -> void { toggle_flag(dbg_flags.overdraw, "overdraw"); });
        // 截图：软件帧缓冲（确定性）/ 真实屏幕窗口（含 OS 装饰，按后端能力）。
        app.shortcuts().add(au::KeyCombo{au::ModifierKey::Control, au::KeyCode::S}, [&app]() -> void {
            if (!app.window()) {
                return;
            }
            auto r = au::debug::capture(app.window()->surface(), "google_play_framebuffer.png");
            auto msg =
                r ? std::string("OK -> aurora_debug/google_play_framebuffer.png") : std::string(r.error().message);
            AURORA_LOG_RAW("demo", "capture(framebuffer): ", msg, "\n");
        });
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位组合，结果为合法组合值而非单枚举量
        constexpr auto ctrl_shift = static_cast<au::ModifierKey>(static_cast<unsigned>(au::ModifierKey::Control) |
                                                                 static_cast<unsigned>(au::ModifierKey::Shift));
        app.shortcuts().add(au::KeyCombo{ctrl_shift, au::KeyCode::S}, [&app]() -> void {
            if (!app.window()) {
                return;
            }
            const auto r = au::debug::capture(app.window()->surface(), "google_play_window.png",
                                              au::debug::CaptureSource::OnScreenWindow);
            auto msg = r ? std::string("OK -> aurora_debug/google_play_window.png") : std::string(r.error().message);
            AURORA_LOG_RAW("demo", "capture(window): ", msg, "\n");
        });
        // 运行时信息：一次性打印全部门面 JSON 到 stdout（人工触发，不刷屏）。
        app.shortcuts().add(au::KeyCombo{au::ModifierKey::Control, au::KeyCode::P}, [&app]() -> void {
            AURORA_LOG_RAW("demo", "=== runtime info (Phase 4) ===\n");
            if (app.window()) {
                AURORA_LOG_RAW("demo", "surface_state:\n", au::debug::surface_state(app.window()->surface()).dump(2),
                               "\n");
            }
            AURORA_LOG_RAW("demo", "widget_tree:\n", au::debug::widget_tree(app.scene().root_node()).dump(2), "\n");
            AURORA_LOG_RAW("demo", "perf_snapshot:\n", au::debug::perf_snapshot().dump(2), "\n");
            AURORA_LOG_RAW("demo", "frame_phase_timeline:\n", au::debug::frame_phase_timeline().dump(2), "\n");
            AURORA_LOG_RAW("demo", "why_trace:\n", au::debug::why_trace().dump(2), "\n");
            AURORA_LOG_RAW("demo", "diagnostics:\n", au::debug::diagnostics().dump(2), "\n");
        });
        AURORA_LOG_RAW("demo",
                       "DEBUG shortcuts: F1-F5 overlays | Ctrl+S framebuffer | Ctrl+Shift+S window | "
                       "Ctrl+P runtime info\n");
    }
#endif

#ifdef AURORA_INSPECTOR_SERVER_ENABLED
    std::shared_ptr<au::InspectorServer> inspector;
    {
        // 远程检视：把 live 根树与运行时 Surface 注入 InspectorServer，启动 localhost HTTP。
        inspector = std::make_shared<au::InspectorServer>([&app]() -> au::Node { return app.scene().root_node(); });
        inspector->set_surface_getter([&app]() -> au::Surface * {
            auto *w = app.window();
            return w ? &w->surface() : nullptr;
        });
        if (inspector->start(6280)) {
            AURORA_LOG_RAW("demo", "InspectorServer: http://127.0.0.1:6280",
                           "  (tree/perf/snapshot/pick/flags live inspection)\n");
        } else {
            AURORA_LOG_RAW("demo", "InspectorServer: failed to start on port 6280\n");
        }
    }
#endif

    app.run();

#ifdef AURORA_INSPECTOR_SERVER_ENABLED
    if (inspector) {
        inspector->stop();
    }
#endif
    return 0;
}