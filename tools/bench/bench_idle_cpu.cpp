// 空闲 CPU 基准（非 CTest）：验证事件驱动帧循环的静态界面 CPU 占用与活跃帧节流。
//
// 场景 ①（idle）：静态文本场景运行 ~3 秒（power_saving 默认开），经 GetProcessTimes
// 计算「进程 CPU 时间 / 墙钟」占比，验收目标 < 5%（改造前忙轮询恒 ≈ 100% 单核）。
// 场景 ②（active）：每帧强制标脏（模拟持续动画），验证帧率被钳制在 max_fps 附近，
// 而非旧忙轮询的不限速空转。
// 用法：./bench_idle_cpu（无 Win32 环境直接跳过，返回 0）
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
// 进程累计 CPU 时间（kernel + user，毫秒）。
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
        return static_cast<double>(u.QuadPart) / 10000.0; // 100ns → ms
    };
    return to_ms(kernel) + to_ms(user);
}

struct RunResult {
    double cpu_ratio = 0.0; ///< 进程 CPU 时间 / 墙钟
    double fps = 0.0;       ///< 有效渲染帧率（idle 跳帧不计）
    double wakeup_per_s = 0.0;
    std::size_t total_frames = 0;
};

// 运行一个场景 duration_ms 后经 WM_CLOSE 退出，返回 CPU 占比与帧统计。
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
    // 到时经 WM_CLOSE 结束帧循环（定时任务本身即唤醒源：idle 循环睡到到期时刻）。
    auto *hwnd = static_cast<HWND>(app.window()->surface().native_handle());
    (void)app.scheduler().set_timeout(std::chrono::milliseconds(duration_ms), [hwnd]() -> void {
        if (hwnd) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
        }
    });
    if (force_redraw_each_frame) {
        // 模拟持续重绘（如视频/每帧变化内容）：关闭脏追踪 → 每帧全绘，
        // has_pending_dirty 恒真 → 活跃帧路径按 max_fps 帧预算节流（而非旧忙轮询不限速）。
        // 注：在 on_frame 里手动标脏会被同帧 present 消费，决策时已无脏；
        // 持续重绘场景的配套 opt-out 即 enable_dirty_tracking(false)（或 power_saving=false）。
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

    // ---- 场景 ①：静态界面 idle（验收：CPU 占比 < 5%）----
    const RunResult idle = run_scenario(false, 3000);
    AURORA_LOG_RAW("bench", "[idle 3s]   cpu ", aurora::bench::ffmt(1, idle.cpu_ratio * 100.0), "% | render fps ",
                   aurora::bench::ffmt(1, idle.fps), " | wakeup/s ", aurora::bench::ffmt(1, idle.wakeup_per_s), "\n");
    if (idle.cpu_ratio >= 0.05) {
        AURORA_LOG_RAW("bench", "[idle 3s]   FAIL: cpu ratio >= 5% (busy loop regression?)\n");
        ++failures;
    } else {
        AURORA_LOG_RAW("bench", "[idle 3s]   PASS: cpu ratio < 5%\n");
    }

    // ---- 场景 ②：持续重绘 active（验收：帧率钳制在 max_fps=60 附近，非无限空转）----
    const RunResult active = run_scenario(true, 3000);
    AURORA_LOG_RAW("bench", "[active 3s] cpu ", aurora::bench::ffmt(1, active.cpu_ratio * 100.0), "% | render fps ",
                   aurora::bench::ffmt(1, active.fps), "\n");
    // 容差放宽（vsync/调度粒度/慢机器）：30 ~ 90 fps 视为「已节流且仍在渲染」。
    if (active.fps < 30.0 || active.fps > 90.0) {
        AURORA_LOG_RAW("bench", "[active 3s] FAIL: fps not clamped near max_fps=60\n");
        ++failures;
    } else {
        AURORA_LOG_RAW("bench", "[active 3s] PASS: fps clamped near max_fps=60\n");
    }

    return failures == 0 ? 0 : 1;
#endif
}
