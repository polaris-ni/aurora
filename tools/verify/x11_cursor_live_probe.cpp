/* 光标形状 —— X11 真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 用途：在**真实 X server** 上验证 `aurora::X11Surface::set_cursor(CursorShape)`
//       确实改变了屏幕上显示的光标（端到端），而不只是「调用不崩」。
//
// 原理：
//   1) 用 aurora 创建真实窗口（被测对象），再用本进程自己的 X 连接做观测；
//   2) 让指针确实落在被测窗口上（先试 XWarpPointer；若被 WM/合成器阻挡，则把窗口
//      临时接管为 override-redirect + 全屏 + 置顶，使指针必然落在其上）；
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
//   3  指针无法落在被测窗口上（典型：Wayland 会话下 rootless Xwayland，X 侧无法定位指针）
//   4  读回恒为同一光标 —— 本会话不支持可靠读回（同属 Wayland/Xwayland 限制）
//   5  部分形状读回相同 —— 疑似读回竞态，请重跑；仍复现则需人工目视复核
//
// 注意：策略 2 会短暂显示一个全屏窗口并改动该窗口的 override-redirect 属性（仅测试侧，
//       不修改被测库代码），退出前撤销；过程中**不移动**用户的物理指针。
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

#include "aurora/window/x11_surface.h"  // aurora 头必须先于 Xlib（None/Bool/Status 宏污染）

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <string>

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

// 轮询命中：指针所在 root 子窗口是否为本窗口（Xwayland 需与合成器往返，故要等待）。
auto wait_pointer_over(Display *dpy, Window root, Window win, int max_ms) -> bool {
    for (int waited = 0; waited <= max_ms; waited += 100) {
        Window child = 0;
        Window returned_root = 0;
        int rx = 0;
        int ry = 0;
        int wx = 0;
        int wy = 0;
        unsigned int mask = 0;
        XSync(dpy, False);
        XQueryPointer(dpy, root, &returned_root, &child, &rx, &ry, &wx, &wy, &mask);
        if (child == win) {
            return true;
        }
        nap_ms(100);
    }
    return false;
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
    AURORA_LOG_RAW("verify", "display=", DisplayString(dpy), " root=",
                   aurora_verify::format_int(screen_w), "x", aurora_verify::format_int(screen_h), " window=",
                   aurora_verify::format_handle(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(win))),
                   "\n");

    // ---- 策略 1：常规映射 + 指针 warp（真实 X11 会话下足够了）----
    XMapRaised(dpy, win);
    XMoveResizeWindow(dpy, win, 60, 60, 480, 320);
    XSync(dpy, False);
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, 60 + 240, 60 + 160);
    bool hit = wait_pointer_over(dpy, root, win, 1500);

    // ---- 策略 2：接管为 override-redirect + 全屏 + 置顶（rootless Xwayland 下必须）----
    bool took_over = false;
    if (!hit) {
        AURORA_LOG_RAW("verify", "Strategy 1 missed (WM redirect / compositor blocking pointer); switching to strategy 2 (fullscreen takeover)\n");
        XUnmapWindow(dpy, win);
        XSetWindowAttributes attrs{};
        attrs.override_redirect = True;
        XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &attrs);
        XMoveResizeWindow(dpy, win, 0, 0, static_cast<unsigned int>(screen_w), static_cast<unsigned int>(screen_h));
        XMapRaised(dpy, win);
        XSync(dpy, False);
        took_over = true;
        hit = wait_pointer_over(dpy, root, win, 3000);
    }

    if (!hit) {
        AURORA_LOG_ERROR("verify",
                         "Pointer cannot be placed over the target window. Typical cause: under a Wayland session, "
                         "rootless Xwayland does not expose pointer position to X clients (XWarpPointer does not move "
                         "the physical pointer). Re-run under an X11 session (not Wayland).");
        XUnmapWindow(dpy, win);
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 3;
    }
    AURORA_LOG_RAW("verify", "Pointer is now over the target window (strategy ", took_over ? 2 : 1, ")\n");

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

    // ---- 撤销接管 ----
    if (took_over) {
        XUnmapWindow(dpy, win);
        XSync(dpy, False);
    }
    XCloseDisplay(dpy);

    const int total = static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT);
    AURORA_LOG_RAW("verify", "Distinct shapes read back=", aurora_verify::format_uint(distinct), " / ",
                   aurora_verify::format_int(total), ", adjacent-equal runs=", aurora_verify::format_int(identical_runs),
                   "\n");
    if (distinct <= 1) {
        AURORA_LOG_ERROR("verify",
                         "Read-back is always the same cursor -- this session cannot reliably read back "
                         "(known Wayland/Xwayland limitation). Re-run this probe under an X11 session.");
        return 4;
    }
    if (distinct < static_cast<std::uint64_t>(total)) {
        AURORA_LOG_ERROR("verify", "Too few distinct shapes read back; possible read-back race -- please re-run; if it reproduces stably, do a manual visual review.");
        return 5;
    }
    AURORA_LOG_RAW("verify", "PASS: all 11 CursorShapes changed the cursor shown on the real X server\n");
    return 0;
}
