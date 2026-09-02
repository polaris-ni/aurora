// Idle-CPU benchmark (not CTest): verifies the CPU usage of the event-driven frame loop on a static
// UI and the throttling of active frames.
//
// Scenario ① (idle): run a static-text scene for ~3 seconds (power_saving on by default) and compute
// the "process CPU time / wall clock" ratio via GetProcessTimes; the acceptance target is < 5%
// (before the rework, busy polling stayed at ~100% of one core).
// Scenario ② (active): force a dirty mark every frame (simulating continuous animation) to verify
// the frame rate is clamped near max_fps instead of the old unbounded busy-polling spin.
// Usage: ./bench_idle_cpu (skipped with return code 0 when there is no Win32 environment)
#include "aurora/aurora.h"

#include "bench_common.h"

#ifdef AURORA_BACKEND_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN // NOLINT(*-identifier-naming)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <chrono>
#include <memory>
#include <string>

namespace {
#ifdef AURORA_BACKEND_WIN32
// Cumulative process CPU time (kernel + user, in milliseconds).
auto process_cpu_ms() -> double {
    FILETIME create{};
    FILETIME exit_t{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit_t, &kernel, &user) == 0) {
        return 0.0;
    }
    const auto to_ms = [](const FILETIME &ft) -> double {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<double>(u.QuadPart) / 10000.0; // 100ns -> ms
    };
    return to_ms(kernel) + to_ms(user);
}

struct RunResult {
    double cpu_ratio = 0.0; ///< process CPU time / wall clock
    double fps = 0.0;       ///< effective render frame rate (idle skipped frames not counted)
    double wakeup_per_s = 0.0;
    std::size_t total_frames = 0;
};

// Run a scenario for duration_ms, then exit via WM_CLOSE; returns the CPU ratio and frame stats.
auto run_scenario(bool force_redraw_each_frame, int duration_ms) -> RunResult {
    au::FrameStats::instance().reset();
    au::Scene scene{ au::Text("bench_idle_cpu: static scene") };
    au::WindowOptions w_opts;
    w_opts.size = au::Size{ .width = 640.0f, .height = 480.0f };
    w_opts.title = force_redraw_each_frame ? "bench_idle_cpu (active)" : "bench_idle_cpu (idle)";
    auto win_res = au::create_window(au::Win32Options{ w_opts });
    if (!win_res) {
        return {};
    }
    au::Application app{ std::move(scene), std::move(win_res.value()), w_opts };
    // End the frame loop at the deadline via WM_CLOSE (the timer task itself is a wakeup source:
    // the idle loop sleeps until the deadline).
    auto *hwnd = static_cast<HWND>(app.window()->surface().native_handle());
    (void)app.scheduler().set_timeout(std::chrono::milliseconds(duration_ms), [hwnd]() -> void {
        if (hwnd) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
        }
    });
    if (force_redraw_each_frame) {
        // Simulate continuous redraw (e.g. video / per-frame changing content): disable dirty
        // tracking -> full paint every frame, has_pending_dirty is always true -> the active-frame
        // path throttles by the max_fps frame budget (instead of the old unbounded busy polling).
        // Note: marking dirty manually inside on_frame is consumed by the same frame's present, so
        // no dirty remains at decision time; the companion opt-out for continuous-redraw scenarios
        // is enable_dirty_tracking(false) (or power_saving=false).
        app.window()->enable_dirty_tracking(false);
    }
    const double cpu0 = process_cpu_ms();
    const auto t0 = std::chrono::steady_clock::now();
    app.run();
    const auto t1 = std::chrono::steady_clock::now();
    const double cpu1 = process_cpu_ms();
    const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    RunResult r;
    r.cpu_ratio = wall_ms > 0.0 ? (cpu1 - cpu0) / wall_ms : 0.0;
    const auto &s = au::FrameStats::instance();
    r.fps = wall_ms > 0.0 ? static_cast<double>(s.total_frames() - s.idle_frame_count()) / (wall_ms / 1000.0) : 0.0;
    r.wakeup_per_s = s.wakeups_per_sec();
    r.total_frames = s.total_frames();
    return r;
}
#endif // AURORA_BACKEND_WIN32
} // namespace

auto main() -> int {
#ifndef AURORA_BACKEND_WIN32
    AURORA_LOG_RAW("bench", "bench_idle_cpu: no Win32 backend, skip\n");
    return 0;
#else
    au::enable_dpi_awareness();
    int failures = 0;

    // ---- scenario 1: static UI idle (acceptance: CPU ratio < 5%) ----
    const RunResult idle = run_scenario(false, 3000);
    AURORA_LOG_RAW("bench", "[idle 3s]   cpu ", aurora::bench::ffmt(1, idle.cpu_ratio * 100.0), "% | render fps ",
                   aurora::bench::ffmt(1, idle.fps), " | wakeup/s ", aurora::bench::ffmt(1, idle.wakeup_per_s), "\n");
    if (idle.cpu_ratio >= 0.05) {
        AURORA_LOG_RAW("bench", "[idle 3s]   FAIL: cpu ratio >= 5% (busy loop regression?)\n");
        ++failures;
    } else {
        AURORA_LOG_RAW("bench", "[idle 3s]   PASS: cpu ratio < 5%\n");
    }

    // ---- scenario 2: continuous-redraw active (acceptance: frame rate clamped near max_fps=60,
    // not an unbounded spin) ----
    const RunResult active = run_scenario(true, 3000);
    AURORA_LOG_RAW("bench", "[active 3s] cpu ", aurora::bench::ffmt(1, active.cpu_ratio * 100.0), "% | render fps ",
                   aurora::bench::ffmt(1, active.fps), "\n");
    // Tolerance is relaxed (vsync / scheduling granularity / slow machines): 30 ~ 90 fps counts as
    // "throttled and still rendering".
    if (active.fps < 30.0 || active.fps > 90.0) {
        AURORA_LOG_RAW("bench", "[active 3s] FAIL: fps not clamped near max_fps=60\n");
        ++failures;
    } else {
        AURORA_LOG_RAW("bench", "[active 3s] PASS: fps clamped near max_fps=60\n");
    }

    return failures == 0 ? 0 : 1;
#endif
}
