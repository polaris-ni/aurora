// tools/verify/win32_window_mode_live_probe.cpp — Win32 家族程序化窗控四槽真机验收探针（非 CTest）。
//
// 覆盖：`Win32Surface` / `D3D11Surface` 的四枚公共入口（minimize / toggle_maximize /
// set_window_mode / set_fullscreen）真的落到 `Win32Host` 的 `SC_*` 目标态腿，并在
// Default / Borderless / Frameless 三档装饰上逐条成立（Borderless / Frameless 是应用自绘
// 标题栏的缺口暴露档：CSD 自用钮路径在该档不可达，此前四槽全部静默 no-op）。
//
// 为什么必须有本探针（无头 CI 证明不了）：判据全部依赖真实 HWND 与消息泵——
//   * `IsIconic` / `IsZoomed` / `GetWindowRect` 是 OS 侧独立取证面，不读库内状态自证；
//   * 上报链（应用级快照与回调）由 `WM_SIZE` → 宿主定档 → `Application` sink 驱动，
//     只有真消息循环下才发生；
//   * 全屏档的「可查询的不支持」要求那条 WARN 真的从宿主发出。
//
// 判据（每档装饰逐条重跑）：
//   (1) `minimize()` → `IsIconic` 为真、应用级快照为 `Minimized`、`set_on_window_mode`
//       回调恰触发一次（值 `Minimized`）；
//   (2) `toggle_maximize()` 在 `Normal` 上 → `IsZoomed` 为真、快照 `Maximized`；再调 →
//       窗口矩形回到调用前逐值（±1px）且快照 `Normal`；
//   (3) `set_window_mode(Maximized)` 连调两次 → 两次后仍 `IsZoomed`、期间上报值不变
//       （目标态幂等，不翻转）；`set_window_mode(Normal)` 还原；
//   (4) `set_fullscreen(true)` → 窗口矩形逐值不变（±1px）、能力位 `runtime_fullscreen`
//       为 false、恰一条 WARN；`set_fullscreen(false)` 不再产生 WARN。
//
// 构建（方式 ① 推荐，Windows 上无需手写编译命令）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BACKEND_WIN32=ON
//         -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_window_mode
//   build-verify\aurora_verify_win32_window_mode.exe
// 运行：命令行直接跑，自动段无需人工干预（会短暂弹出并操作三扇测试窗口）。
// 退出码：0 全过；1 有判据失败；2 环境不可用（后端未开 / 建窗失败 / D3D11 设备不可用）。
// 人工段（另行目视）：任务栏最小化/还原动画、Borderless 自绘标题栏场景的按钮行为，
//   见 codespec/manual-test/10-window.md。

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_window_mode can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/widget/text.h"
#include "aurora/window/platform.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#elif defined(AURORA_BACKEND_D3D11)
#include "aurora/window/d3d11_surface.h"
#else
#error "aurora_verify_win32_window_mode requires AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11"
#endif

namespace {

/// @brief 本探针使用的上屏面类型：Win32 优先，仅开 D3D11 时用 D3D11Surface（同一宿主）。
#ifdef AURORA_BACKEND_WIN32
using ProbeSurface = aurora::Win32Surface;
#else
using ProbeSurface = aurora::D3D11Surface;
#endif

/// @brief 累计的断言失败条数，决定退出码 1。
auto failures() -> int & {
    static int count = 0;
    return count;
}

/// @brief 输出一行探针结论到 stdout（经 `AURORA_LOG_RAW`：无日志前缀、不过级别过滤）。
auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        ++failures();
    }
}

/// @brief RAII 日志捕获：接管 Logger sink，统计含给定片段的行数（判据 4 要证「恰一条 WARN」）。
class CapturedLog {
  public:
    CapturedLog() {
        aurora::Logger::instance().set_sink([this](std::string_view line) -> void { lines_.emplace_back(line); });
    }
    ~CapturedLog() { aurora::Logger::instance().set_sink(nullptr); }
    CapturedLog(const CapturedLog &) = delete;
    auto operator=(const CapturedLog &) -> CapturedLog & = delete;
    CapturedLog(CapturedLog &&) = delete;
    auto operator=(CapturedLog &&) -> CapturedLog & = delete;

    /// @brief 统计含给定片段的已捕获行数。
    /// @param fragment 待匹配片段（大小写敏感，ASCII）。
    /// @return 命中行数。
    [[nodiscard]] auto count(std::string_view fragment) const -> int {
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [fragment](const std::string &l) {
            return l.find(fragment) != std::string::npos;
        }));
    }

  private:
    std::vector<std::string> lines_;
};

/// @brief 抽干本线程消息队列（SC_* 是 Post 语义，WM_SIZE 回报要泵完才到）。
auto pump_messages() -> void {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/// @brief 取窗口矩形（OS 侧独立取证面）。
[[nodiscard]] auto window_rect(HWND hwnd) -> RECT {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    return rc;
}

/// @brief 两矩形逐值比较（判据允许 OS 边框 ±1px 的取整抖动）。
[[nodiscard]] auto rect_close(const RECT &a, const RECT &b) -> bool {
    const auto near1 = [](long x, long y) { return x - y <= 1 && y - x <= 1; };
    return near1(a.left, b.left) && near1(a.top, b.top) && near1(a.right, b.right) && near1(a.bottom, b.bottom);
}

/// @brief 一档装饰上的完整四判据段落。
/// @param decoration 本档装饰策略。
/// @param label 档名（打印归因用，ASCII）。
/// @return true 正常跑完判据；false 环境不可用（建窗失败 / D3D11 设备不可用）。
auto run_one_decoration(aurora::DecorationPolicy decoration, const std::string &label) -> bool {
    emit("-- decoration: " + label + " --");

    aurora::WindowStyleOptions style{};
    style.resizable = true;
    style.decoration = decoration;
    auto surface = std::make_unique<ProbeSurface>(640, 480, "Aurora win32 window mode probe", style);
#ifndef AURORA_BACKEND_WIN32
    if (!surface->is_available()) {
        emit("result: D3D11 device unavailable (exit 2)");
        return false;
    }
#endif
    HWND hwnd = static_cast<HWND>(surface->hwnd());
    if (hwnd == nullptr) {
        check(false, "(" + label + ") a native window could be created");
        return false;
    }
    aurora::Scene scene{aurora::Node{std::make_shared<aurora::Text>("hi")}};
    aurora::Application app{std::move(scene), std::move(surface)};

    std::vector<aurora::WindowMode> reported;
    app.set_on_window_mode([&reported](aurora::WindowMode m) -> void { reported.push_back(m); });
    pump_messages();

    // 能力位自证：本家族公共入口已接通三原语、全屏档是可查询的不支持。
    const aurora::PlatformCapabilities caps = aurora::platform().capabilities();
    check(caps.runtime_window_mode, "(" + label + ") capabilities.runtime_window_mode is true for this backend");
    check(!caps.runtime_fullscreen, "(" + label + ") capabilities.runtime_fullscreen is false for this backend");

    // (1) minimize()：OS 实值 + 应用级快照 + 回调恰一次。
    reported.clear();
    app.window()->minimize();
    pump_messages();
    check(IsIconic(hwnd) != FALSE, "(" + label + ") minimize() really iconized the window (IsIconic)");
    check(app.window_mode().get() == aurora::WindowMode::Minimized,
          "(" + label + ") minimize() reported Minimized to the application snapshot");
    check(reported.size() == 1 && reported.at(0) == aurora::WindowMode::Minimized,
          "(" + label + ") minimize() fired set_on_window_mode exactly once with Minimized");

    // 还原腿（set_window_mode 目标态语义）。
    app.window()->surface().set_window_mode(aurora::WindowMode::Normal);
    pump_messages();
    check(IsIconic(hwnd) == FALSE, "(" + label + ") set_window_mode(Normal) restored the window");

    // (2) toggle_maximize() 两连发：最大化 → 还原到调用前矩形。
    reported.clear();
    const RECT before = window_rect(hwnd);
    app.window()->toggle_maximize();
    pump_messages();
    check(IsZoomed(hwnd) != FALSE, "(" + label + ") toggle_maximize() on Normal really zoomed (IsZoomed)");
    check(app.window_mode().get() == aurora::WindowMode::Maximized,
          "(" + label + ") toggle_maximize() reported Maximized to the application snapshot");
    app.window()->toggle_maximize();
    pump_messages();
    check(rect_close(window_rect(hwnd), before),
          "(" + label + ") second toggle_maximize() restored the pre-toggle window rect (+-1px)");
    check(app.window_mode().get() == aurora::WindowMode::Normal,
          "(" + label + ") second toggle_maximize() reported Normal to the application snapshot");

    // (3) set_window_mode 目标态幂等：连调两次不翻转。
    reported.clear();
    app.window()->surface().set_window_mode(aurora::WindowMode::Maximized);
    pump_messages();
    app.window()->surface().set_window_mode(aurora::WindowMode::Maximized);
    pump_messages();
    check(IsZoomed(hwnd) != FALSE, "(" + label + ") set_window_mode(Maximized) twice keeps the window zoomed");
    check(app.window_mode().get() == aurora::WindowMode::Maximized,
          "(" + label + ") repeated set_window_mode(Maximized) never flipped the reported mode");
    check(std::all_of(reported.begin(), reported.end(),
                      [](aurora::WindowMode m) { return m == aurora::WindowMode::Maximized; }),
          "(" + label + ") every report during the double request stayed Maximized (no toggle)");
    app.window()->surface().set_window_mode(aurora::WindowMode::Normal);
    pump_messages();
    check(IsZoomed(hwnd) == FALSE, "(" + label + ") set_window_mode(Normal) un-zoomed the window");

    // (4) set_fullscreen：矩形不动 + 恰一条 WARN + off 无 WARN。
    const RECT pre_fullscreen = window_rect(hwnd);
    {
        CapturedLog log;
        app.window()->set_fullscreen(true);
        pump_messages();
        check(rect_close(window_rect(hwnd), pre_fullscreen),
              "(" + label + ") set_fullscreen(true) left the window rect untouched (+-1px)");
        check(log.count("WRN") == 1,
              "(" + label + ") set_fullscreen(true) produced exactly one WARN (no silent swallow)");
    }
    {
        CapturedLog log;
        app.window()->set_fullscreen(false);
        pump_messages();
        check(log.count("WRN") == 0,
              "(" + label + ") set_fullscreen(false) is a quiet no-op (never entered fullscreen)");
    }
    return true;  // Application 析构销毁窗口
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常落到非零退出码，捕获反而把它压成 0（与 demo 入口同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    aurora::init_console();  // Windows 控制台切 UTF-8，避免表格错行
    emit("== Aurora Win32 window-mode wiring live probe ==");

    // 三档装饰逐条重跑（Borderless / Frameless 是应用自绘标题栏的缺口暴露档）。
    const bool ok_default = run_one_decoration(aurora::DecorationPolicy::Auto, "default");
    const bool ok_borderless = run_one_decoration(aurora::DecorationPolicy::Borderless, "borderless");
    const bool ok_frameless = run_one_decoration(aurora::DecorationPolicy::Frameless, "frameless");
    if (!ok_default || !ok_borderless || !ok_frameless) {
        return 2;  // 环境不可用（建窗失败 / D3D11 设备不可用）
    }

    if (failures() == 0) {
        emit("PASS: Win32 window-mode wiring accepted on default/borderless/frameless decorations");
        return 0;
    }
    emit("result: one or more criteria failed (exit 1)");
    return 1;
}
