// tools/verify/x11_geometry_live_probe.cpp — X11 缩放上报 + 窗口几何真机探针（非 CTest）。
//
// 覆盖：X11 后端两条**只能在真实 X server 上证明**的接线——
//   (1) 运行期 DPI 变化上报：改写根窗口 `RESOURCE_MANAGER` 的 `Xft.dpi` → 下一轮事件泵后
//       `scale_factor()` 反映新值、`WindowHost::on_scale_changed()` 触发的
//       `force_full_redraw()` 真的产出新帧（`frame_count()` 自增），且逻辑尺寸按新 scale 重算；
//   (2) 窗口几何 setter：显式 `set_size(逻辑 dp)` / `set_position(物理 px)` 后，
//       用**独立 Xlib 观测**（`XGetGeometry` / `XTranslateCoordinates`）断言窗口真实改变，
//       而非只「调用不崩」。
//
// 为什么必须有本探针：CI 无头环境既没有 X server、也无法造出「Xft.dpi 变了」这一事件；
// 而 §233–292 的几何持久化恢复在 X11 上长期是 no-op（基类空实现），单测只能证明「调用了」，
// 证明不了「窗口真的动了」。故本探针不进 CTest，由人工在真实会话触发。
//
// 自动段（无需人工，任何 X11 / XWayland 会话均可跑）：
//   (a) 基线：会话可建窗、帧泵成功、`scale_factor()` 与当前 `Xft.dpi` 一致；
//   (b) 缩放上报端到端（**保留 WindowHost 接好的回调**）：写一个新 dpi → 断言
//       `scale_factor()` == 新 dpi/96、`frame_count()` 自增（force_full_redraw 真的出帧）、
//       `Window::size() × 新 scale ≈ XGetGeometry 物理尺寸`；
//   (c) 几何 setter：`set_size(640×420 dp)` → XGetGeometry 物理尺寸 == round(dp × scale)，
//       `Surface::size()` 同步；`set_position(+60,+40 物理 px)` → 独立观测的客户区原点位移
//       落在请求量附近（容差覆盖重父化 WM 的装饰/重力常量偏移，见 (c4) 注释），且
//       `Surface::position()` 与独立观测同值；
//   (d) 回调通道 + 去重：自行注册 `set_scale_change_handler` 后再改 dpi → handler 被调用且
//       收到新值 == `scale_factor()`；再写**同值** → 断言不再上报（`next != scale` 唯一拦截点）。
//
// 几何断言按**真实时间轮询落地**：`XResizeWindow` / `XMoveWindow` 只是发给 WM 的请求，由 WM /
// 合成器决定何时生效（WSLg 的 Weston WM 实测 0.1–1 s）。旧版「推两帧即断言」会假红。
//
// 判据刻意不依赖 `xdotool`：Xlib 观测与本进程同一 X 连接，既避免外部工具缺失导致的
// ENV-UNAVAILABLE，也避免「读回来的其实是另一个窗口」的假阳性。人工目视复核建议：
// 运行 `xdotool getwindowgeometry --shell <XID>` 对照探针打印的 XID 与几何。
//
// 构建：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BACKEND_X11=ON
//         -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_x11_geometry
// 运行：build-verify 目录下 ./aurora_verify_x11_geometry
//
// 退出码：0 自动段全过；1 有断言失败；2 环境不可用（无 DISPLAY / 建窗失败 / 无 X 连接）。
// 本探针全自动，无人工段；`--interactive` 仅为与其余探针共用入口而接受（不改变判据）。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_x11_geometry can only be built on Linux/Unix (non-Apple)"
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
        failures()++;
    }
}

auto nearly(float a, float b, float tol = 0.02F) -> bool { return std::fabs(a - b) <= tol; }

/// @brief 根窗口几何观测（客户区物理尺寸 + 客户区左上角的根窗口坐标）。
struct Geometry {
    unsigned int width = 0;
    unsigned int height = 0;
    int root_x = 0;
    int root_y = 0;
};

/// @brief 独立观测：XGetGeometry 取物理尺寸，XTranslateCoordinates 取客户区屏幕原点。
auto observe_geometry(Display *dpy, ::Window win) -> std::optional<Geometry> {
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
    return Geometry{.width = cw, .height = ch, .root_x = translated ? rx : xr, .root_y = translated ? ry : yr};
}

/// @brief 轮询等待 WM / 合成器把几何请求真正落地。
///
/// X11 的 `XResizeWindow` / `XMoveWindow` 只是**发给 WM 的请求**（ICCCM ConfigureRequest），由 WM
/// 转交、合成器（rootless XWayland 下是 Wayland 合成器）决定何时生效。WSLg 的 Weston WM 实测
/// 延迟 0.1–1 s；`Session::pump` 只推帧不等真实时间，故必须按真实时间轮询，否则「请求刚发出、
/// 尚未落地」会被误判为 setter 无效（本探针首版即因此假红）。
/// @param session E2E 会话：轮询期间持续泵平台事件，让 ConfigureNotify 进入表面缓存。
/// @param dpy 独立观测连接。
/// @param win 被测窗口（XID）。
/// @param done 落地判据（满足即返回）。
/// @param timeout_ms 轮询上限（毫秒）。
/// @return 最后一次观测到的几何；超时未满足判据时调用方按失败处理。
template <typename Done>
auto wait_for_geometry(aurora::e2e::Session &session, Display *dpy, ::Window win, Done done, int timeout_ms = 3000)
    -> std::optional<Geometry> {
    std::optional<Geometry> last;
    for (int waited = 0; waited <= timeout_ms; waited += 50) {
        session.pump_events();
        last = observe_geometry(dpy, win);
        if (last.has_value() && done(*last)) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return last;
}

/// @brief 窗口是否已被 WM 放置到真实屏幕坐标。
///
/// rootless XWayland（WSLg / GNOME Wayland）在 Weston WM 完成首次摆放前，`XTranslateCoordinates`
/// 会报一个近似 `INT16_MIN` 的哨兵坐标；此时任何位置判据都无意义，须先等放置完成。
auto is_placed(const Geometry &g) -> bool { return g.root_x > -1000 && g.root_y > -1000; }

/// @brief 读取根窗口 RESOURCE_MANAGER 原文（保存/恢复用）；不存在时返回 nullopt。
auto read_root_resource(Display *dpy) -> std::optional<std::string> {
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = nullptr;
    const Status ok = XGetWindowProperty(dpy, DefaultRootWindow(dpy), XA_RESOURCE_MANAGER, 0L, 100000L, False,
                                         XA_STRING, &actual_type, &actual_format, &nitems, &bytes_after, &prop);
    std::optional<std::string> out;
    if (ok == Success && actual_type == XA_STRING && actual_format == 8 && prop != nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        out = std::string(reinterpret_cast<const char *>(prop), nitems);
    }
    if (prop != nullptr) {
        XFree(prop);
    }
    return out;
}

auto write_root_resource(Display *dpy, const std::string &text) -> void {
    XChangeProperty(dpy, DefaultRootWindow(dpy), XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(text.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                    static_cast<int>(text.size()));
    XSync(dpy, False);
}

/// @brief 还原探针改动过的根窗口资源串（RAII）：无论断言是否失败都不把用户的 Xft.dpi 留在测试值。
struct ResourceManagerGuard {
    Display *dpy = nullptr;
    std::optional<std::string> original;

    ~ResourceManagerGuard() {
        if (dpy == nullptr) {
            return;
        }
        if (original.has_value()) {
            write_root_resource(dpy, *original);
        } else {
            XDeleteProperty(dpy, DefaultRootWindow(dpy), XA_RESOURCE_MANAGER);
            XSync(dpy, False);
        }
    }

    ResourceManagerGuard() = default;
    ResourceManagerGuard(Display *display, std::optional<std::string> saved)
        : dpy(display), original(std::move(saved)) {}
    ResourceManagerGuard(const ResourceManagerGuard &) = delete;
    auto operator=(const ResourceManagerGuard &) -> ResourceManagerGuard & = delete;
    ResourceManagerGuard(ResourceManagerGuard &&) = delete;
    auto operator=(ResourceManagerGuard &&) -> ResourceManagerGuard & = delete;
};

/// @brief dpi → 与真实桌面同形的资源串（制表符分隔，带尾换行）。
auto dpi_resource(int dpi) -> std::string { return "Xft.dpi:\t" + std::to_string(dpi) + "\n"; }

/// @brief 选一个与当前缩放不同的整数 dpi，保证「变化才上报」路径真的被驱动。
auto alternate_dpi(float current_scale) -> int { return nearly(current_scale, 1.5F) ? 192 : 144; }

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现（口径同 tools/verify/ 其余探针）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("X11 geometry + DPI scale-report live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    (void)cli.arguments->flag("interactive");  // 本探针全自动，旗标仅为统一入口而接受

    emit("== aurora verify: X11 geometry + DPI scale reporting ==");

    aurora::e2e::WindowSpec spec;
    spec.backend = aurora::e2e::Backend::X11;
    spec.width = 480;
    spec.height = 320;
    spec.title = "aurora-verify-x11-geometry";
    spec.visibility = aurora::WindowVisibility::Normal;  // 几何 setter 需要 WM 真实参与
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
    // native_handle 契约返回 void *，X11 的 XID 本是整数：void* → uintptr_t → Window 只用于
    // 喂给独立观测的 Xlib 调用，从不解引用。就地豁免。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto xwin = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(surface->native_handle()));
    if (dpy == nullptr || xwin == 0) {
        AURORA_LOG_ERROR("verify", "no native X display/window handle (environment unavailable)");
        return 2;
    }
    ResourceManagerGuard guard(dpy, read_root_resource(dpy));

    // 挂最小控件树：`Session::pump` 要求已 mount；`Row` 只提供可布局实体，不参与尺寸判据。
    aurora::Node root = aurora::Node{aurora::Row{}};
    session.mount(root);

    // ---- (a) 基线 ----
    const auto settled = session.pump_until_settled(10);
    check(settled.ok(),
          "(a0) baseline frames settled" + (settled.ok() ? std::string{} : ": " + settled.error().message));
    auto geo = observe_geometry(dpy, xwin);
    check(geo.has_value(), "(a1) independent Xlib geometry observation works");
    if (!geo.has_value()) {
        return 1;
    }
    const float base_scale = surface->scale_factor();
    emit("  window XID = " + aurora_verify::format_hex(static_cast<std::uint64_t>(xwin)) +
         ", base scale_factor() = " + std::to_string(base_scale));
    check(nearly(win.size().width * base_scale, static_cast<float>(geo->width), 1.5F),
          "(a2) Surface::size() x scale == XGetGeometry physical width (" + std::to_string(win.size().width) + " x " +
              std::to_string(base_scale) + " vs " + aurora_verify::format_uint(geo->width) + ")");

    // ---- (b) 缩放上报端到端：改 Xft.dpi → scale_factor 跟上 + force_full_redraw 真出帧 ----
    const int dpi1 = alternate_dpi(base_scale);
    const float want_scale = static_cast<float>(dpi1) / 96.0F;
    const int frames_before = session.frame_count();
    write_root_resource(dpy, dpi_resource(dpi1));
    const auto pumped = session.pump(3);
    check(pumped.ok(), "(b0) frame pump after Xft.dpi change succeeded");
    check(nearly(surface->scale_factor(), want_scale), "(b1) scale_factor() reflects the new Xft.dpi (" +
                                                           std::to_string(surface->scale_factor()) + " vs " +
                                                           std::to_string(want_scale) + ")");
    // 回调链（WindowHost::on_scale_changed → force_full_redraw）若断了，改 dpi 不会标脏，
    // 此后的帧会被脏追踪判为 idle 跳过、present 不执行，frame_count 不动。
    check(session.frame_count() > frames_before, "(b2) scale change triggered a redraw (frame_count " +
                                                     std::to_string(frames_before) + " -> " +
                                                     std::to_string(session.frame_count()) + ")");
    geo = observe_geometry(dpy, xwin);
    check(geo.has_value(), "(b3) geometry readable after scale change");
    if (geo.has_value()) {
        check(nearly(win.size().width * surface->scale_factor(), static_cast<float>(geo->width), 1.5F),
              "(b4) logical size recomputed at the new scale (physical " + aurora_verify::format_uint(geo->width) +
                  "px / " + std::to_string(surface->scale_factor()) + " == " + std::to_string(win.size().width) + ")");
    }

    // ---- (c) set_size / set_position 真的改变窗口几何 ----
    // 先等 WM 完成首次摆放：未放置的 rootless 窗口报哨兵坐标，位置判据无意义。
    const auto placed = wait_for_geometry(session, dpy, xwin, is_placed);
    check(placed.has_value() && is_placed(*placed), "(c0) window was placed by the WM (root coords " +
                                                        std::to_string(placed ? placed->root_x : 0) + "," +
                                                        std::to_string(placed ? placed->root_y : 0) + ")");

    const float scale_now = surface->scale_factor();
    const aurora::Size target_dp{.width = 640.0F, .height = 420.0F};
    const auto expect_w = static_cast<unsigned int>(std::lround(target_dp.width * scale_now));
    const auto expect_h = static_cast<unsigned int>(std::lround(target_dp.height * scale_now));
    surface->set_size(target_dp);
    // 尺寸请求同样经 WM 转交：按落地判据轮询，而不是「推两帧就断言」。
    const auto sized = wait_for_geometry(
        session, dpy, xwin, [&](const Geometry &g) -> bool { return g.width == expect_w && g.height == expect_h; });
    check(sized.has_value() && sized->width == expect_w && sized->height == expect_h,
          "(c1) set_size(dp) reached the window as physical px (got " +
              aurora_verify::format_uint(sized ? sized->width : 0) + "x" +
              aurora_verify::format_uint(sized ? sized->height : 0) + ", want " + std::to_string(expect_w) + "x" +
              std::to_string(expect_h) + ")");
    check(nearly(win.size().width, target_dp.width, 1.5F) && nearly(win.size().height, target_dp.height, 1.5F),
          "(c2) Surface::size() follows set_size (" + std::to_string(win.size().width) + "x" +
              std::to_string(win.size().height) + ")");

    const auto before_move = observe_geometry(dpy, xwin);
    check(before_move.has_value(), "(c3) geometry readable before set_position");
    if (before_move.has_value()) {
        constexpr int dx = 60;
        constexpr int dy = 40;
        // 容差覆盖 WM 的装饰/重力常量偏移：重父化 WM 可能把「客户区移到 (x,y)」解释为
        // 「框架移到 (x,y)」并加上固定的边框/阴影量（WSLg 的 Weston WM 实测 +6/+27）。
        // 容差仍需远小于请求量，故「请求根本没送达」表现为位移≈0、必然判红。
        constexpr int k_gravity_tolerance = 48;
        const aurora::Point want{.x = static_cast<float>(before_move->root_x + dx),
                                 .y = static_cast<float>(before_move->root_y + dy)};
        surface->set_position(want);
        // 先等窗口真的动起来（移动请求落地），再等 200 ms 让 WM 的最终位置稳定后复测。
        const auto moved = wait_for_geometry(session, dpy, xwin, [&](const Geometry &g) -> bool {
            return g.root_x != before_move->root_x || g.root_y != before_move->root_y;
        });
        const bool moved_at_all =
            moved.has_value() && (moved->root_x != before_move->root_x || moved->root_y != before_move->root_y);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        session.pump_events();
        const auto final_geo = observe_geometry(dpy, xwin);
        const int got_dx = final_geo.has_value() ? final_geo->root_x - before_move->root_x : 0;
        const int got_dy = final_geo.has_value() ? final_geo->root_y - before_move->root_y : 0;
        const bool shifted = moved_at_all && std::abs(got_dx - dx) <= k_gravity_tolerance &&
                             std::abs(got_dy - dy) <= k_gravity_tolerance;
        check(shifted, "(c4) set_position moved the window by the requested delta (got " + std::to_string(got_dx) +
                           "," + std::to_string(got_dy) + ", want " + std::to_string(dx) + "," + std::to_string(dy) +
                           ", tolerance " + std::to_string(k_gravity_tolerance) + ")");
        const aurora::Point reported = surface->position();
        check(final_geo.has_value() && nearly(reported.x, static_cast<float>(final_geo->root_x), 1.0F) &&
                  nearly(reported.y, static_cast<float>(final_geo->root_y), 1.0F),
              "(c5) Surface::position() agrees with independent observation (" + std::to_string(reported.x) + "," +
                  std::to_string(reported.y) + ")");
    }

    // ---- (d) 回调通道 + 去重 ----
    int report_count = 0;
    float reported_scale = 0.0F;
    win.surface().set_scale_change_handler([&report_count, &reported_scale](float s) -> void {
        report_count++;
        reported_scale = s;
    });
    const int dpi2 = (dpi1 == 144) ? 192 : 144;
    const float want_scale2 = static_cast<float>(dpi2) / 96.0F;
    write_root_resource(dpy, dpi_resource(dpi2));
    (void)session.pump(2);
    check(report_count > 0,
          "(d1) registered scale-change handler was called (count=" + std::to_string(report_count) + ")");
    check(nearly(reported_scale, want_scale2), "(d2) handler received the new scale (" +
                                                   std::to_string(reported_scale) + " vs " +
                                                   std::to_string(want_scale2) + ")");
    check(nearly(surface->scale_factor(), reported_scale), "(d3) reported scale == scale_factor() (" +
                                                               std::to_string(reported_scale) + " vs " +
                                                               std::to_string(surface->scale_factor()) + ")");

    // 同值再写一次：XChangeProperty 仍产生 PropertyNotify，去重只能由实现里的
    // `if (next != d.scale)` 拦住——删掉该判断本条即转红。
    const int before_dup = report_count;
    write_root_resource(dpy, dpi_resource(dpi2));
    (void)session.pump(1);
    check(report_count == before_dup, "(d4) duplicate dpi notification is deduped (count " +
                                          std::to_string(before_dup) + " -> " + std::to_string(report_count) + ")");

    if (failures() == 0) {
        emit("== automatic segment passed ==");
        emit("  manual cross-check (optional): xdotool getwindowgeometry --shell " +
             aurora_verify::format_hex(static_cast<std::uint64_t>(xwin)));
        return 0;
    }
    emit("== automatic segment FAILED (" + std::to_string(failures()) + ") ==");
    return 1;
}
