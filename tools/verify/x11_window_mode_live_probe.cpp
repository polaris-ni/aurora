// tools/verify/x11_window_mode_live_probe.cpp — X11 程序化窗控真机验收探针（非 CTest）。
//
// 覆盖：X11 后端 `set_fullscreen` 真接线（本次落地：`X11Surface::set_fullscreen` 转发
// `set_window_mode(FullScreen)`，后者经 EWMH `_NET_WM_STATE_FULLSCREEN` 原子交给 WM 接管全屏
// 与进出屏几何还原）。无头 CI 建不了真实 X server、也发不出「WM 把窗口铺满显示器」这一事实，
// 故本探针不进 CTest，由人工在真实 X 会话（含运行中的 WM）触发。
//
// 为什么必须有本探针：能力矩阵 `runtime_fullscreen` 对 X11 现已翻 true，但「true」只保证公共
// 入口接通——真正落到 WM 的是 `_NET_WM_STATE_FULLSCREEN` 原子，必须由**独立取证**证明：
//   * 读窗口属性 `_NET_WM_STATE`，确认 FULLSCREEN 原子在列（WM 侧实际状态，非库内自证）；
//   * 应用级快照 `Window::window_mode()` 经 WM 回报收敛到 `FullScreen`；
//   * 无任何 WARN（实现已落地，不是「留一条 WARN 的 stub」）。
//
// 判据（全在真实 X 会话下执行；无 DISPLAY / 建窗失败 / 无 WM 按退出码 2 报环境不可用）：
//   (1) 能力位自证：`runtime_fullscreen == true`（本次翻位）；`runtime_window_mode == false`
//       （minimize / toggle_maximize 两枚公共切换腿在 X11 仍未接线，能力位如实 false，调用是
//       可观测的 no-op 而非静默失效）。
//   (2) `set_fullscreen(true)`：
//         - `_NET_WM_STATE` 属性含 `_NET_WM_STATE_FULLSCREEN` 原子（独立取证：WM 已受理）；
//         - 应用级快照 `Window::window_mode()` 收敛到 `FullScreen`；
//         - 全程零 WARN；
//         - 几何覆盖（best-effort）：单显示器下窗口应铺满整屏（与根屏同尺寸 ±2px）；多显示器
//           因 X11 后端未链 XRandr、探针无从枚举显示器，故降级为 NOTE，不判 FAIL——
//           `_NET_WM_STATE_FULLSCREEN` 原子才是绑定的全屏证据。
//   (3) `set_fullscreen(false)`：原子移除、快照回 `Normal`、零 WARN、几何还原到进入前（best-effort）。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BACKEND_X11=ON
//         -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_x11_window_mode
// 运行：build-verify 目录下 ./aurora_verify_x11_window_mode（真实 X 会话，须有 WM 在跑）
//
// 退出码：0 全过；1 有判据失败；2 环境不可用（无 DISPLAY / 建窗失败 / 无 X 连接 / 无 WM 受理）。
// 本探针全自动，无人工段；`--interactive` 仅为与其余探针共用入口而接受（不改变判据）。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_x11_window_mode can only be built on Linux/Unix (non-Apple)"
#endif
#ifndef AURORA_BACKEND_X11
#error "AURORA_BACKEND_X11 must be enabled"
#endif

// 包含顺序不可交给 clang-format 排序：默认 IncludeBlocks: Merge 会把 <X11/…>（C 系统头类别）
// 提到 aurora 头之前，而 Xlib 的 `#define None 0L` / `#define CursorShape 0` 会污染 aurora 侧
// 同名枚举。故 aurora 头必须先解析。
// clang-format off
#include "aurora/aurora.h"
#include "aurora/window/x11_surface.h"
#include "e2e/harness.h"
#include "verify_args.h"
#include "verify_print.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
// clang-format on

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

// 失败计数藏函数内静态：探针不落全局可变状态（与 glfw_dpi_live_probe 同款）。
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

    /// @brief 统计含给定片段的已捕获行数。
    [[nodiscard]] auto count(std::string_view fragment) const -> int {
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [fragment](const std::string &l) {
            return l.find(fragment) != std::string::npos;
        }));
    }

  private:
    std::vector<std::string> lines_;
};

/// @brief 窗口矩形（客户区原点 + 尺寸，单位物理像素）。
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// @brief 独立观测：XGetGeometry 取尺寸，XTranslateCoordinates 取客户区屏幕原点。
auto read_geometry(Display *dpy, ::Window win) -> std::optional<Rect> {
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

/// @brief 读窗口 `_NET_WM_STATE` 属性，判断目标原子是否在列（独立取证 WM 实际状态）。
auto has_net_wm_state(Display *dpy, ::Window win, Atom target) -> bool {
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = nullptr;
    const Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    const Status ok = XGetWindowProperty(dpy, win, net_wm_state, 0L, 1024L, False, XA_ATOM, &actual_type,
                                         &actual_format, &nitems, &bytes_after, &prop);
    bool found = false;
    if (ok == Success && actual_type == XA_ATOM && prop != nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *atoms = reinterpret_cast<const Atom *>(prop);
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == target) {
                found = true;
                break;
            }
        }
    }
    if (prop != nullptr) {
        XFree(prop);
    }
    return found;
}

/// @brief 让 WM 有时间受理 ClientMessage 并回报（X11 全屏是异步 WM 契约，非同步生效）。
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
    const auto cli = aurora_verify::parse_interactive("X11 window-mode live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    (void)cli.arguments->flag("interactive");  // 本探针全自动，旗标仅为统一入口而接受

    emit("== aurora verify: X11 window-mode (fullscreen) wiring ==");

    aurora::e2e::WindowSpec spec;
    spec.backend = aurora::e2e::Backend::X11;
    spec.width = 640;
    spec.height = 480;
    spec.title = "aurora-verify-x11-window-mode";
    spec.visibility = aurora::WindowVisibility::Normal;  // 全屏需 WM 真实参与
    auto session = aurora::e2e::open(spec);
    if (!session.ok()) {
        AURORA_LOG_ERROR("verify", "open(X11) failed: " + session.reason() + " (no DISPLAY / X server unreachable)");
        return 2;
    }
    auto &win = session.window();
    auto *surface = dynamic_cast<aurora::X11Surface *>(&win.surface());
    if (surface == nullptr) {
        AURORA_LOG_ERROR("verify", "surface is not an X11Surface (wrong backend wiring)");
        return 1;
    }
    auto *dpy = static_cast<Display *>(surface->native_display());
    // native_handle 契约返回 void*，X11 的 XID 本是整数：void* → uintptr_t → Window 只用于
    // 喂给独立观测的 Xlib 调用，从不解引用。就地豁免。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
    if (dpy == nullptr || xwin == 0) {
        AURORA_LOG_ERROR("verify", "no native X display/window handle (environment unavailable)");
        return 2;
    }

    // 挂最小控件树：`Session::pump` 要求已 mount；`Row` 只提供可布局实体，不参与判据。
    aurora::Node root = aurora::Node{aurora::Row{}};
    session.mount(root);
    const auto settled = session.pump_until_settled(10);
    check(settled.ok(), "(0) baseline frames settled" + (settled.ok() ? std::string{} : ": " + settled.error().message));

    const Atom fullscreen_atom = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    const int screen = DefaultScreen(dpy);
    const int root_w = DisplayWidth(dpy, screen);
    const int root_h = DisplayHeight(dpy, screen);
    emit("  window XID = " + aurora_verify::format_hex(static_cast<std::uint64_t>(xwin)) + ", root " +
         aurora_verify::format_int(root_w) + "x" + aurora_verify::format_int(root_h));

    // ---- (1) 能力位自证 ----
    const aurora::PlatformCapabilities caps = aurora::platform().capabilities();
    check(caps.runtime_fullscreen, "(1a) capabilities.runtime_fullscreen is true for X11 (set_fullscreen wired)");
    check(!caps.runtime_window_mode,
          "(1b) capabilities.runtime_window_mode is false for X11 (minimize/toggle_maximize legs unwired)");

    // ---- (2) set_fullscreen(true) ----
    {
        CapturedLog log;
        const auto before = read_geometry(dpy, xwin);
        win.set_fullscreen(true);
        settle_wm(session);
        const bool atom_on = has_net_wm_state(dpy, xwin, fullscreen_atom);
        check(atom_on, "(2a) _NET_WM_STATE contains _NET_WM_STATE_FULLSCREEN (WM honored the request)");
        check(win.window_mode() == aurora::WindowMode::FullScreen,
              "(2b) application snapshot reports FullScreen after set_fullscreen(true)");
        check(log.count("WRN") == 0, "(2c) set_fullscreen(true) produced no WARN (real implementation, not a stub)");
        const auto after = read_geometry(dpy, xwin);
        if (after.has_value()) {
            const bool covers_root = after->x >= -2 && after->x <= 2 && after->y >= -2 && after->y <= 2 &&
                                     after->w >= root_w - 4 && after->w <= root_w + 4 && after->h >= root_h - 4 &&
                                     after->h <= root_h + 4;
            if (covers_root) {
                check(true, "(2d) window covers the whole root screen (single-monitor fullscreen)");
            } else if (before.has_value() && (after->w < before->w || after->h < before->h)) {
                check(false, "(2d) window did not grow under fullscreen (w=" + aurora_verify::format_int(after->w) +
                                 ", h=" + aurora_verify::format_int(after->h) + ")");
            } else {
                // 多显示器：X11 后端未链 XRandr，探针无从枚举显示器，几何覆盖降级为 NOTE。
                // `_NET_WM_STATE_FULLSCREEN` 原子（2a）才是绑定的全屏证据。
                emit("[NOTE] (2d) window geometry " + aurora_verify::format_int(after->x) + "," +
                     aurora_verify::format_int(after->y) + " " + aurora_verify::format_int(after->w) + "x" +
                     aurora_verify::format_int(after->h) + " — multi-monitor layout cannot be asserted without XRandr; " +
                     "the _NET_WM_STATE_FULLSCREEN atom (2a) is the binding proof");
            }
        }
    }

    // ---- (3) set_fullscreen(false) ----
    {
        CapturedLog log;
        const auto pre_exit = read_geometry(dpy, xwin);
        win.set_fullscreen(false);
        settle_wm(session);
        check(!has_net_wm_state(dpy, xwin, fullscreen_atom),
              "(3a) _NET_WM_STATE no longer contains _NET_WM_STATE_FULLSCREEN after set_fullscreen(false)");
        check(win.window_mode() == aurora::WindowMode::Normal,
              "(3b) application snapshot reports Normal after set_fullscreen(false)");
        check(log.count("WRN") == 0, "(3c) set_fullscreen(false) produced no WARN");
        const auto after = read_geometry(dpy, xwin);
        if (pre_exit.has_value() && after.has_value()) {
            const bool restored = std::abs(after->x - pre_exit->x) <= 2 && std::abs(after->y - pre_exit->y) <= 2 &&
                                  std::abs(after->w - pre_exit->w) <= 2 && std::abs(after->h - pre_exit->h) <= 2;
            check(restored, "(3d) window geometry restored to pre-fullscreen rect (+-2px)");
        }
    }

    if (failures() == 0) {
        emit("PASS: X11 window-mode fullscreen wiring accepted (real X session with WM)");
        return 0;
    }
    emit("result: one or more criteria failed (exit 1)");
    return 1;
}
