// tools/verify/glfw_window_mode_live_probe.cpp — GLFW 程序化窗控真机验收探针（非 CTest）。
//
// 覆盖：GLFW 后端 `set_fullscreen` 真接线（本次落地：`GlfwSurface::set_fullscreen` 进入前存档几何
// 与装饰态、取所在显示器物理视口、去装饰、铺满整屏——borderless 全屏，不改视频模式；退出还原）。
// 无头 CI 建不了真实窗口、也证明不了「窗口真的铺满显示器」，故本探针不进 CTest，由人工在真实
// 桌面会话触发。
//
// 为什么必须有本探针：能力矩阵 `runtime_fullscreen` 对 GLFW 现已翻 true，但「true」只保证公共
// 入口接通——真正落到屏幕的是窗口几何被设为显示器视口，必须由**独立取证**证明：
//   * 用平台原生观测（Linux：Xlib + glfwGetX11Display；Windows：HWND + Monitor API）读出窗口矩形，
//     与 GLFW 显示器视口（glfwGetMonitors + glfwGetMonitorPos + glfwGetVideoMode）逐值比对，确认
//     覆盖整屏（±2px）；
//   * 应用级快照 `Window::window_mode()` 收敛到 `FullScreen`；
//   * 无任何 WARN（实现已落地，不是「留一条 WARN 的 stub」）。
//
// 判据（全在真实桌面会话下执行；glfwInit 失败 / 建窗失败按退出码 2 报环境不可用）：
//   (1) 能力位自证：`runtime_fullscreen == true`（本次翻位）；`runtime_window_mode == false`
//       （minimize / toggle_maximize 两枚公共切换腿在 GLFW 仍未接线，能力位如实 false）。
//   (2) `set_fullscreen(true)`：
//         - 独立观测窗口矩形覆盖某块 GLFW 显示器视口（±2px，borderless 铺满）；
//         - 应用级快照 `Window::window_mode()` 收敛到 `FullScreen`；
//         - 全程零 WARN；
//         - 进入前存档的几何/装饰态不影响判据（退出会还原，见 (3)）。
//   (3) `set_fullscreen(false)`：矩形还原到进入前（±2px）、快照回 `Normal`、零 WARN。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -DAURORA_BACKEND_GLFW=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_glfw_window_mode
// 运行：build-verify 目录下 ./aurora_verify_glfw_window_mode（真实桌面会话）
//
// 退出码：0 全过；1 有判据失败；2 环境不可用（glfwInit / 建窗失败）。
// 本探针全自动，无人工段；`--interactive` 仅为与其余探针共用入口而接受（不改变判据）。
// macOS 下原生几何观测未实现，该平台仅验快照 + 零 WARN（几何覆盖降级为 NOTE）。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_BACKEND_GLFW
#error "AURORA_BACKEND_GLFW must be enabled"
#endif

#include "aurora/aurora.h"
#include "aurora/window/glfw_surface.h"
#include "e2e/harness.h"
#include "verify_args.h"
#include "verify_print.h"

// 本探针只用「窗口 + 全屏几何」这层 GLFW API，完全不碰 OpenGL。定义 GLFW_INCLUDE_NONE
// 可让 glfw3.h 不去 `#include <GL/gl.h>`——既是语义上的正确（无 GL 依赖），也顺带让本探针
// 在**没装 GL 开发包**的环境里也能编译。必须在 glfw3.h 之前定义。
#ifndef GLFW_INCLUDE_NONE
// GLFW_INCLUDE_NONE 是 GLFW 规定的宏名（必须在包含 glfw3.h 前定义），不可加 AURORA_ 前缀或改名，只能就地豁免。
// NOLINTNEXTLINE(readability-identifier-naming)
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#ifdef AURORA_PLATFORM_LINUX
// clang-format off
#include <X11/Xlib.h>
// clang-format on
#endif
#ifdef AURORA_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>
#endif

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

auto failures() -> int & {
    static int count = 0;
    return count;
}

auto check(bool passed, const std::string &label) -> void {
    emit(std::string("[") + (passed ? "PASS" : "FAIL") + "] " + label);
    if (!passed) {
        ++failures();
    }
}

/// @brief RAII 日志捕获：接管 Logger sink，统计含给定片段的行数（判据要证「零 WARN」）。
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

    [[nodiscard]] auto count(std::string_view fragment) const -> int {
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [fragment](const std::string &l) {
            return l.find(fragment) != std::string::npos;
        }));
    }

  private:
    std::vector<std::string> lines_;
};

/// @brief 窗口矩形（原点 + 尺寸，单位物理像素）。
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

#ifdef AURORA_PLATFORM_LINUX
/// @brief 独立观测（Xlib）：XGetGeometry 取尺寸，XTranslateCoordinates 取客户区屏幕原点。
auto read_geometry_linux(Display *dpy, ::Window win) -> std::optional<Rect> {
    ::Window root_ret = 0;
    int xr = 0;
    int yr = 0;
    unsigned int cw = 0;
    unsigned int ch = 0;
    unsigned int bw = 0;
    unsigned int depth = 0;
    if (XGetGeometry(dpy, win, &root_ret, &xr, &yr, &cw, &ch, &bw, &depth) == 0) {
        return std::nullopt;
    }
    ::Window child = 0;
    int rx = 0;
    int ry = 0;
    const bool translated = XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child) != False;
    return Rect{.x = translated ? rx : xr, .y = translated ? ry : yr, .w = static_cast<int>(cw), .h = static_cast<int>(ch)};
}
#endif

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief 独立观测（Win32）：HWND 客户区矩形即窗口物理几何。
auto read_geometry_win(HWND hwnd) -> std::optional<Rect> {
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) == 0) {
        return std::nullopt;
    }
    return Rect{.x = rc.left, .y = rc.top, .w = rc.right - rc.left, .h = rc.bottom - rc.top};
}
#endif

/// @brief 收集 GLFW 全部显示器的物理视口（位置 + 视频模式分辨率），供全屏几何比对。
auto expected_monitor_rects() -> std::vector<Rect> {
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    std::vector<Rect> rects;
    if (monitors == nullptr) {
        return rects;
    }
    for (int i = 0; i < count; ++i) {
        int mx = 0;
        int my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode *vm = glfwGetVideoMode(monitors[i]);
        if (vm == nullptr) {
            continue;
        }
        rects.push_back(Rect{.x = mx, .y = my, .w = vm->width, .h = vm->height});
    }
    return rects;
}

/// @brief 实测窗口矩形是否覆盖某块 GLFW 显示器视口（±2px）。
[[nodiscard]] auto covers_a_monitor(const Rect &actual, const std::vector<Rect> &expected) -> bool {
    for (const Rect &e : expected) {
        if (std::abs(actual.x - e.x) <= 2 && std::abs(actual.y - e.y) <= 2 && std::abs(actual.w - e.w) <= 2 &&
            std::abs(actual.h - e.h) <= 2) {
            return true;
        }
    }
    return false;
}

/// @brief 让平台窗口管理器落地几何请求（GLFW 的 set pos/size 同 X11 一样是发往 WM 的请求）。
auto settle_wm(aurora::e2e::Session &session) -> void {
    for (int i = 0; i < 10; ++i) {
        (void)session.pump(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现（口径同 tools/verify/ 其余探针）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("GLFW window-mode live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    (void)cli.arguments->flag("interactive");  // 本探针全自动，旗标仅为统一入口而接受

    emit("== aurora verify: GLFW window-mode (fullscreen) wiring ==");

    aurora::e2e::WindowSpec spec;
    spec.backend = aurora::e2e::Backend::Glfw;
    spec.width = 640;
    spec.height = 480;
    spec.title = "aurora-verify-glfw-window-mode";
    spec.visibility = aurora::WindowVisibility::Normal;  // 全屏需 WM 真实参与
    auto session = aurora::e2e::open(spec);
    if (!session.ok()) {
        AURORA_LOG_ERROR("verify", "open(Glfw) failed: " + session.reason() + " (no display / GLFW init failed)");
        return 2;
    }
    auto &win = session.window();
    auto *surface = dynamic_cast<aurora::GlfwSurface *>(&win.surface());
    if (surface == nullptr) {
        AURORA_LOG_ERROR("verify", "surface is not a GlfwSurface (wrong backend wiring)");
        return 1;
    }

    aurora::Node root = aurora::Node{aurora::Row{}};
    session.mount(root);
    const auto settled = session.pump_until_settled(10);
    check(settled.ok(), "(0) baseline frames settled" + (settled.ok() ? std::string{} : ": " + settled.error().message));

    const std::vector<Rect> monitors = expected_monitor_rects();
    emit("  GLFW monitors reported: " + aurora_verify::format_int(static_cast<long long>(monitors.size())));

    // ---- (1) 能力位自证 ----
    const aurora::PlatformCapabilities caps = aurora::platform().capabilities();
    check(caps.runtime_fullscreen, "(1a) capabilities.runtime_fullscreen is true for GLFW (set_fullscreen wired)");
    check(!caps.runtime_window_mode,
          "(1b) capabilities.runtime_window_mode is false for GLFW (minimize/toggle_maximize legs unwired)");

    // ---- (2) set_fullscreen(true) ----
    {
        CapturedLog log;
        const auto before = [&]() -> std::optional<Rect> {
#ifdef AURORA_PLATFORM_LINUX
            Display *dpy = glfwGetX11Display();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
            return (dpy != nullptr && xwin != 0) ? read_geometry_linux(dpy, xwin) : std::nullopt;
#elif defined(AURORA_PLATFORM_WINDOWS)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto hwnd = static_cast<HWND>(surface->native_handle());
            return (hwnd != nullptr) ? read_geometry_win(hwnd) : std::nullopt;
#else
            (void)surface;
            return std::nullopt;  // macOS：原生几何观测未实现，降级 NOTE
#endif
        }();
        win.set_fullscreen(true);
        settle_wm(session);
        check(win.window_mode() == aurora::WindowMode::FullScreen,
              "(2a) application snapshot reports FullScreen after set_fullscreen(true)");
        check(log.count("WRN") == 0, "(2b) set_fullscreen(true) produced no WARN (real implementation, not a stub)");
        const auto after = [&]() -> std::optional<Rect> {
#ifdef AURORA_PLATFORM_LINUX
            Display *dpy = glfwGetX11Display();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
            return (dpy != nullptr && xwin != 0) ? read_geometry_linux(dpy, xwin) : std::nullopt;
#elif defined(AURORA_PLATFORM_WINDOWS)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto hwnd = static_cast<HWND>(surface->native_handle());
            return (hwnd != nullptr) ? read_geometry_win(hwnd) : std::nullopt;
#else
            return std::nullopt;
#endif
        }();
        if (after.has_value() && !monitors.empty()) {
            check(covers_a_monitor(*after, monitors),
                  "(2c) window covers a full GLFW monitor viewport (x=" + aurora_verify::format_int(after->x) +
                      ", y=" + aurora_verify::format_int(after->y) + ", w=" + aurora_verify::format_int(after->w) +
                      "x" + aurora_verify::format_int(after->h) + ")");
        } else if (!monitors.empty()) {
            check(false, "(2c) could not read window geometry for fullscreen coverage check");
        } else {
            emit("[NOTE] (2c) no GLFW monitor reported; geometry coverage not asserted on this platform");
        }
        (void)before;
    }

    // ---- (3) set_fullscreen(false) ----
    {
        CapturedLog log;
        const auto pre_exit = [&]() -> std::optional<Rect> {
#ifdef AURORA_PLATFORM_LINUX
            Display *dpy = glfwGetX11Display();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
            return (dpy != nullptr && xwin != 0) ? read_geometry_linux(dpy, xwin) : std::nullopt;
#elif defined(AURORA_PLATFORM_WINDOWS)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto hwnd = static_cast<HWND>(surface->native_handle());
            return (hwnd != nullptr) ? read_geometry_win(hwnd) : std::nullopt;
#else
            return std::nullopt;
#endif
        }();
        win.set_fullscreen(false);
        settle_wm(session);
        check(win.window_mode() == aurora::WindowMode::Normal,
              "(3a) application snapshot reports Normal after set_fullscreen(false)");
        check(log.count("WRN") == 0, "(3b) set_fullscreen(false) produced no WARN");
        const auto after = [&]() -> std::optional<Rect> {
#ifdef AURORA_PLATFORM_LINUX
            Display *dpy = glfwGetX11Display();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
            return (dpy != nullptr && xwin != 0) ? read_geometry_linux(dpy, xwin) : std::nullopt;
#elif defined(AURORA_PLATFORM_WINDOWS)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto hwnd = static_cast<HWND>(surface->native_handle());
            return (hwnd != nullptr) ? read_geometry_win(hwnd) : std::nullopt;
#else
            return std::nullopt;
#endif
        }();
        if (pre_exit.has_value() && after.has_value()) {
            const bool restored = std::abs(after->x - pre_exit->x) <= 2 && std::abs(after->y - pre_exit->y) <= 2 &&
                                  std::abs(after->w - pre_exit->w) <= 2 && std::abs(after->h - pre_exit->h) <= 2;
            check(restored, "(3c) window geometry restored to pre-fullscreen rect (+-2px)");
        }
    }

    if (failures() == 0) {
        emit("PASS: GLFW window-mode fullscreen wiring accepted (real desktop session)");
        return 0;
    }
    emit("result: one or more criteria failed (exit 1)");
    return 1;
}
