/* 光标形状 —— X11 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 用途：在**真实 X server** 上验证 `aurora::X11Surface::set_cursor(CursorShape)`
//       确实改变了屏幕上显示的光标（端到端），而不只是「调用不崩」。
//
// 原理：
//   1) 用 aurora 创建真实窗口（被测对象），再用本进程自己的 X 连接做观测；
//   2) 让指针确实落在被测窗口上：读回窗口自身的 **root 相对几何**（等其稳定且完整落在 root
//      内），按该几何的中心 XWarpPointer，再用同一次几何读回判定命中（强判据 XQueryPointer
//      的 child==win；rootless Xwayland 常不回 child，故退一步按几何包含放行）；
//   3) 对 11 个 CursorShape 逐个 set_cursor，用 XFIXES 的 XFixesGetCursorImage 读回
//      「屏幕上真实显示的光标」的尺寸 / 热点 / 名称 / 像素哈希，判定是否逐个改变。
//
// 构建（方式 ① CMake 目标，推荐）：
//   cmake -S . -B build-verify -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DAURORA_BACKEND_X11=ON -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_x11_cursor
//   ./build-verify/aurora_verify_x11_cursor
// 构建（方式 ② 手写命令，须先有一个已构建好的 X11 后端构建目录 build-x11）：
//   g++ -std=c++20 -DAURORA_BACKEND_X11 -DAURORA_BACKEND_HEADLESS -DAURORA_ENABLE_SIMD \
//       -I include -I src -I third_party -I tools/verify \
//       -I build-x11/third_party/freetype/include -I third_party/harfbuzz/src \
//       tools/verify/x11_cursor_live_probe.cpp -o /tmp/x11_cursor_live_probe \
//       build-x11/libaurora.a build-x11/third_party/harfbuzz/libharfbuzz.a \
//       build-x11/third_party/freetype/libfreetype.a -lX11 -lz -lpthread -ldl -lm
//
// 运行：
//   DISPLAY=:0 ./aurora_verify_x11_cursor            # 默认用环境里的 DISPLAY
//   DISPLAY=:0 ./aurora_verify_x11_cursor :1         # 也可显式指定
//
// 退出码：
//   0  全部 11 个形状读回两两互异 —— 真机验收通过
//   2  环境缺依赖（无 X 连接 / 无 libXfixes / 无 XFIXES 扩展）
//   3  指针无法落在被测窗口上（取不到稳定几何，或合成器把窗口整块放到 root 之外）
//   4  读回恒为同一光标 —— 本会话不支持可靠读回（典型：rootless Xwayland 下 X 侧光标与
//      Wayland 合成器实际显示的光标脱钩）
//   5  部分形状读回相同 —— 疑似读回竞态，请重跑；仍复现则需人工目视复核
//
// 注意：策略 2 只是把窗口改成居中小窗并 raise（**不**再改 override-redirect——WSLg 实测
//       unmap+接管+重映射会让窗口在观测连接上长期停在非 IsViewable 态，反而堵死落点判定）。
//       本探针不移动用户的物理指针离开被测窗口，退出前关闭观测连接；落点判定按几何放行时，
//       区分真伪的仍是第 3 步的 XFIXES 读回，故不会因此给出假阳性。
//   本仓库实测（2026-09-20，WSLg rootless Xwayland，7680x2160）：策略 1 即以几何放行命中
//       （`XQueryPointer` 回 child≠win，指针由合成器侧持有），11 形状读回 11/11 互异且逐行
//       等于 `x11_cursor_glyph` 期望字形（left_ptr / xterm / hand2 / sb_v_double_arrow /
//       sb_h_double_arrow / top_left_corner / top_right_corner / fleur / crosshair /
//       X_cursor，`wait` 为动画光标故 name 空、serial 跳变）——屏幕光标已证明。
//   构建命令中的行尾反斜杠为续行符，故本头注释整体使用块注释形态（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_UNIX) || defined(AURORA_PLATFORM_MACOS)
#error "aurora_verify_x11_cursor can only be built on Linux/Unix (non-Apple)"
#endif
#if !defined(AURORA_BACKEND_X11)
#error "AURORA_BACKEND_X11 must be enabled"
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

#include "aurora/window/x11_surface.h"  // aurora 澶村繀椤诲厛浜?Xlib锛圢one/Bool/Status 瀹忔薄鏌擄級

// <X11/X.h>（经 Xlib.h 引入）无条件 `#define CursorShape 0`，与 aurora::CursorShape 硬碰撞。
#undef CursorShape

#include "aurora/window/cursor_map.h"
#include "verify_print.h"

namespace {

// XFIXES `XFixesCursorImage` 的 ABI 前缀（v1 字段 + v2 追加字段）。
// 多数发行版只装 libXfixes 运行库、不装开发包，故按 ABI 手写声明 + dlopen 取符号。
struct XFixesCursorImageAbi {
    short x = 0;
    short y = 0;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned short xhot = 0;
    unsigned short yhot = 0;
    unsigned long cursor_serial = 0;
    unsigned long *pixels = nullptr;
    unsigned long atom = 0;
    const char *name = nullptr;
};

using GetCursorImageFn = XFixesCursorImageAbi *(*)(Display *);
using QueryExtensionFn = int (*)(Display *, int *, int *);

GetCursorImageFn g_get_cursor_image = nullptr;

auto hash_pixels(const XFixesCursorImageAbi *image) -> std::uint64_t {
    std::uint64_t h = 1469598103934665603ULL;  // FNV-1a 64
    const auto count = static_cast<std::size_t>(image->width) * static_cast<std::size_t>(image->height);
    for (std::size_t i = 0; i < count; ++i) {
        h ^= static_cast<std::uint64_t>(image->pixels[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

void nap_ms(long ms) {
    struct timespec ts{};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000L * 1000L;
    nanosleep(&ts, nullptr);
}

struct CursorSnapshot {
    std::uint64_t hash = 0;
    std::string name;
    unsigned short width = 0;
    unsigned short height = 0;
    unsigned short xhot = 0;
    unsigned short yhot = 0;
    unsigned long serial = 0;
};

// 同步 + 稳定读回：读两次一致才采信（消除「服务器尚未应用」造成的陈旧值）。
auto read_cursor_settled(Display *dpy) -> CursorSnapshot {
    CursorSnapshot out;
    for (int attempt = 0; attempt < 20; ++attempt) {
        XSync(dpy, False);
        nap_ms(25);
        XFixesCursorImageAbi *first = g_get_cursor_image(dpy);
        if (first == nullptr) {
            return out;
        }
        const std::uint64_t h1 = hash_pixels(first);
        const std::string n1 = first->name == nullptr ? std::string() : std::string(first->name);
        XFree(first);
        XSync(dpy, False);
        nap_ms(10);
        XFixesCursorImageAbi *second = g_get_cursor_image(dpy);
        if (second == nullptr) {
            return out;
        }
        const std::uint64_t h2 = hash_pixels(second);
        const std::string n2 = second->name == nullptr ? std::string() : std::string(second->name);
        if (h1 == h2 && n1 == n2) {
            out.hash = h1;
            out.name = n1;
            out.width = second->width;
            out.height = second->height;
            out.xhot = second->xhot;
            out.yhot = second->yhot;
            out.serial = second->cursor_serial;
            XFree(second);
            return out;
        }
        XFree(second);
    }
    return out;
}

// 指针所在 root 子窗口是否为本窗口（Xwayland 需与合成器往返，故要轮询等待）。
// 两级判据：
//   * 强判据：`XQueryPointer` 回 `child == win`——真实 X11 会话下必然可用；
//   * 几何判据：rootless Xwayland 下 `XQueryPointer` 常回 `child = None`（指针由 Wayland
//     合成器持有，X 侧只做 root 坐标映射），故退一步按「指针 root 坐标落在本窗口几何内」
//     放行。
// 放行本身不构成假阳性：真正的判据是随后的 XFIXES 读回——若本窗口并未持有该处光标，读回会
// 恒为同一光标而按退出码 4 失败，绝不会因此报 PASS。
// 几何证据（诊断用，-1 = 取不到）：窗口在 root 上的左上角、自身尺寸与映射态。
struct WinGeom {
    int x = -1;
    int y = -1;
    int w = -1;
    int h = -1;
    int mapped = -1;  // 1 = IsViewable
};

// 窗口的 root 相对几何。origin 走 XTranslateCoordinates（WM reparent 后 attrs.x/y 只是
// 父窗口相对值），size/映射态走 XGetWindowAttributes。任一步失败（窗口失效）返回 false。
// 两者都可能对刚被合成器接管的窗口偶发报错，故调用方须容忍 false 并重试。
auto win_geometry(Display *dpy, Window root, Window win, WinGeom *out) -> bool {
    Window child = 0;
    int tx = 0;
    int ty = 0;
    const bool translated = XTranslateCoordinates(dpy, win, root, 0, 0, &tx, &ty, &child) != 0;
    XWindowAttributes attrs{};
    const bool queried = XGetWindowAttributes(dpy, win, &attrs) != 0;
    if (out != nullptr) {
        out->x = translated ? tx : -1;
        out->y = translated ? ty : -1;
        out->w = queried ? attrs.width : -1;
        out->h = queried ? attrs.height : -1;
        out->mapped = queried && attrs.map_state == IsViewable ? 1 : 0;
    }
    return translated && queried && attrs.width > 0 && attrs.height > 0;
}

auto pointer_inside_geom(int rx, int ry, const WinGeom &g) -> bool {
    return g.w > 0 && rx >= g.x && rx < g.x + g.w && ry >= g.y && ry < g.y + g.h;
}

// 把指针放到被测窗口上：先等窗口几何**稳定且完整落在 root 内**，再按该几何的中心 warp，
// 最后用**同一次**几何读回做命中判定。
//
// 为什么不能按硬编码坐标 warp：X 客户端的 XMoveResizeWindow 只是「请求」，WM/合成器会把
// 窗口挪到别处（WSLg 实测：请求全屏 (0,0,7680x2160) 后窗口报回左上角 (639,1214)），此时
// 按固定点 warp 与按另一份几何判定互相对不上，探针会以 rc=3 假性宣告「指针放不过去」。
// 这里 warp 目标与判定依据同源，且几何未稳定前不落点，故判定反映的是真实接线。
//
// 返回 true 时 `*by_geometry` 说明命中的是哪一级判据；`*geom` / `*out_x/y/child` 是现场证据。
auto place_pointer_over(Display *dpy, Window root, Window win, int screen_w, int screen_h, WinGeom *geom, int *out_x,
                        int *out_y, Window *out_child, bool *by_geometry) -> bool {
    for (int attempt = 0; attempt < 6; ++attempt) {
        XRaiseWindow(dpy, win);
        XSync(dpy, False);
        WinGeom g{};
        WinGeom prev{};
        bool stable = false;
        // 等 WM 把窗口安放到最终位置：连续两次读回一致且完整落在 root 内才算稳定。
        for (int i = 0; i < 12 && !stable; ++i) {
            if (!win_geometry(dpy, root, win, &g) || g.x < 0 || g.y < 0 || g.x + g.w > screen_w ||
                g.y + g.h > screen_h) {
                nap_ms(80);
                continue;
            }
            stable = i > 0 && g.x == prev.x && g.y == prev.y && g.w == prev.w && g.h == prev.h;
            prev = g;
            if (!stable) {
                nap_ms(80);
            }
        }
        if (!stable) {
            continue;  // 几何取不到 / 一直在动 / 被放到 root 外：重读一次整体再来
        }
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, g.x + g.w / 2, g.y + g.h / 2);
        XSync(dpy, False);
        for (int waited = 0; waited <= 800; waited += 100) {
            Window child = 0;
            Window returned_root = 0;
            int rx = 0;
            int ry = 0;
            int wx = 0;
            int wy = 0;
            unsigned int mask = 0;
            XSync(dpy, False);
            XQueryPointer(dpy, root, &returned_root, &child, &rx, &ry, &wx, &wy, &mask);
            const bool strong = child == win;
            const bool geo = !strong && pointer_inside_geom(rx, ry, g);
            if (strong || geo) {
                *geom = g;
                *out_x = rx;
                *out_y = ry;
                *out_child = child;
                *by_geometry = !strong;
                return true;
            }
            nap_ms(100);
        }
    }
    win_geometry(dpy, root, win, geom);
    Window child = 0;
    Window returned_root = 0;
    int rx = 0;
    int ry = 0;
    int wx = 0;
    int wy = 0;
    unsigned int mask = 0;
    XSync(dpy, False);
    XQueryPointer(dpy, root, &returned_root, &child, &rx, &ry, &wx, &wy, &mask);
    *out_x = rx;
    *out_y = ry;
    *out_child = child;
    *by_geometry = false;
    return false;
}

// Xlib 默认错误处理会直接 exit(1)。观测侧的偶发 BadMatch/BadWindow（窗口刚被 unmap 或
// reparent）不该让探针无声消失，故改为记录后继续——真伪判据仍由 XFIXES 读回把守。
auto x_error_handler(Display *dpy, XErrorEvent *ev) -> int {
    char text[128] = {0};
    XGetErrorText(dpy, ev->error_code, text, sizeof(text) - 1);
    AURORA_LOG_WARN("verify", std::string("X error on observation connection (ignored): ") + text +
                                  " (opcode=" + aurora_verify::format_int(ev->request_code) + ")");
    return 0;
}

}  // namespace

auto main(int argc, char **argv) -> int {
    if (argc > 1) {
        ::setenv("DISPLAY", argv[1], 1);
    }

    aurora::X11Surface surface(240, 160, "aurora-verify-i1-cursor-x11");
    if (!surface.is_available()) {
        AURORA_LOG_ERROR("verify", "X11Surface unavailable -- no DISPLAY or no usable X server");
        return 2;
    }
    const auto win = static_cast<Window>(reinterpret_cast<std::uintptr_t>(surface.native_handle()));

    Display *dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        AURORA_LOG_ERROR("verify", "Observation-side XOpenDisplay failed");
        return 2;
    }
    XSetErrorHandler(x_error_handler);
    void *lib = dlopen("libXfixes.so.3", RTLD_NOW);
    if (lib == nullptr) {
        lib = dlopen("libXfixes.so", RTLD_NOW);
    }
    if (lib == nullptr) {
        AURORA_LOG_ERROR("verify", "Missing libXfixes (runtime lib); cannot read back cursor");
        XCloseDisplay(dpy);
        return 2;
    }
    g_get_cursor_image = reinterpret_cast<GetCursorImageFn>(dlsym(lib, "XFixesGetCursorImage"));
    auto query_extension = reinterpret_cast<QueryExtensionFn>(dlsym(lib, "XFixesQueryExtension"));
    int event_base = 0;
    int error_base = 0;
    if (g_get_cursor_image == nullptr || query_extension == nullptr ||
        query_extension(dpy, &event_base, &error_base) == 0) {
        AURORA_LOG_ERROR("verify", "XFIXES extension unavailable");
        XCloseDisplay(dpy);
        return 2;
    }

    const Window root = DefaultRootWindow(dpy);
    const int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    const int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    AURORA_LOG_RAW("verify", "display=", DisplayString(dpy), " root=", aurora_verify::format_int(screen_w), "x",
                   aurora_verify::format_int(screen_h), " window=",
                   aurora_verify::format_handle(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(win))),
                   "\n");

    // ---- 策略 1：常规映射 + 按窗口自身几何 warp（真实 X11 会话下 child==win 即命中）----
    XMapRaised(dpy, win);
    XMoveResizeWindow(dpy, win, 60, 60, 480, 320);
    XSync(dpy, False);
    int px = -1;
    int py = -1;
    Window pchild = 0;
    bool by_geometry = false;
    WinGeom geom;
    bool hit = place_pointer_over(dpy, root, win, screen_w, screen_h, &geom, &px, &py, &pchild, &by_geometry);

    // ---- 策略 2：改请求一个居中的小窗（合成器更易把它完整安放进屏内），再按几何落点 ----
    bool took_over = false;
    if (!hit) {
        AURORA_LOG_RAW("verify", "Strategy 1 missed (child=", aurora_verify::format_uint(pchild), " root=(",
                       aurora_verify::format_int(px), ",", aurora_verify::format_int(py), ") geom=(",
                       aurora_verify::format_int(geom.x), ",", aurora_verify::format_int(geom.y), ",",
                       aurora_verify::format_int(geom.w), "x", aurora_verify::format_int(geom.h),
                       ", mapped=", aurora_verify::format_int(geom.mapped),
                       "); switching to strategy 2 (centered modest window)\n");
        const int mw = screen_w > 900 ? 800 : screen_w / 2;
        const int mh = screen_h > 700 ? 600 : screen_h / 2;
        XMoveResizeWindow(dpy, win, (screen_w - mw) / 2, (screen_h - mh) / 2, static_cast<unsigned int>(mw),
                          static_cast<unsigned int>(mh));
        XRaiseWindow(dpy, win);
        XSync(dpy, False);
        took_over = true;
        hit = place_pointer_over(dpy, root, win, screen_w, screen_h, &geom, &px, &py, &pchild, &by_geometry);
    }

    if (!hit) {
        AURORA_LOG_ERROR("verify",
                         "Pointer cannot be placed over the target window: the window geometry never settled fully "
                         "inside the root, or XWarpPointer did not take effect. Evidence: root child=" +
                             aurora_verify::format_uint(pchild) + " root_point=(" + aurora_verify::format_int(px) +
                             "," + aurora_verify::format_int(py) + ") screen=" + aurora_verify::format_int(screen_w) +
                             "x" + aurora_verify::format_int(screen_h) + " win_geom=(" +
                             aurora_verify::format_int(geom.x) + "," + aurora_verify::format_int(geom.y) + "," +
                             aurora_verify::format_int(geom.w) + "x" + aurora_verify::format_int(geom.h) +
                             ", mapped=" + aurora_verify::format_int(geom.mapped) + ")");
        XUnmapWindow(dpy, win);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 3;
    }
    AURORA_LOG_RAW("verify", "Pointer is over the target window (strategy ", took_over ? 2 : 1,
                   by_geometry ? ", accepted by geometry; XQueryPointer returned child=None (rootless Xwayland)"
                               : ", accepted by XQueryPointer child match",
                   ", root=(", aurora_verify::format_int(px), ",", aurora_verify::format_int(py),
                   ", child=", aurora_verify::format_uint(pchild), ")\n");

    // ---- 11 形状逐个下发 + 读回 ----
    AURORA_LOG_RAW("verify", aurora_verify::pad_right("shape(rfc name)", 20), aurora_verify::pad_right("w", 5),
                   aurora_verify::pad_right("h", 5), aurora_verify::pad_right("xhot", 6),
                   aurora_verify::pad_right("yhot", 6), aurora_verify::pad_right("serial", 10),
                   aurora_verify::pad_right("xfixed_name", 22), "pixel_hash\n");
    std::uint64_t distinct = 0;
    std::uint64_t last_hash = 0;
    int identical_runs = 0;
    for (int i = 0; i < static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT); ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        surface.set_cursor(shape);
        const CursorSnapshot snap = read_cursor_settled(dpy);
        AURORA_LOG_RAW("verify", aurora_verify::pad_right(aurora::cursor_rfc_name(shape), 20),
                       aurora_verify::pad_right(aurora_verify::format_uint(snap.width), 5),
                       aurora_verify::pad_right(aurora_verify::format_uint(snap.height), 5),
                       aurora_verify::pad_right(aurora_verify::format_uint(snap.xhot), 6),
                       aurora_verify::pad_right(aurora_verify::format_uint(snap.yhot), 6),
                       aurora_verify::pad_right(aurora_verify::format_uint(snap.serial), 10),
                       aurora_verify::pad_right(snap.name.empty() ? "<empty>" : snap.name, 22),
                       aurora_verify::format_uint(snap.hash), "\n");
        if (i == 0 || snap.hash != last_hash) {
            ++distinct;
        } else {
            ++identical_runs;
        }
        last_hash = snap.hash;
    }

    // ---- 收尾：指针放回原处由被测窗口析构负责（本探针不改动物理指针之外的用户状态）----
    XSync(dpy, False);
    XCloseDisplay(dpy);

    const int total = static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT);
    AURORA_LOG_RAW("verify", "Distinct shapes read back=", aurora_verify::format_uint(distinct), " / ",
                   aurora_verify::format_int(total),
                   ", adjacent-equal runs=", aurora_verify::format_int(identical_runs), "\n");
    if (distinct <= 1) {
        AURORA_LOG_ERROR("verify",
                         "Read-back is always the same cursor -- this session cannot reliably read back "
                         "(known Wayland/Xwayland limitation). Re-run this probe under an X11 session.");
        return 4;
    }
    if (distinct < static_cast<std::uint64_t>(total)) {
        AURORA_LOG_ERROR("verify",
                         "Too few distinct shapes read back; possible read-back race -- please re-run; if it "
                         "reproduces stably, do a manual visual review.");
        return 5;
    }
    AURORA_LOG_RAW("verify", "PASS: all 11 CursorShapes changed the cursor shown on the real X server\n");
    return 0;
}
