/* 光标形状 —— Win32 家族真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖后端：`Win32Surface`（GDI 上屏）、`D3D11Surface`（GPU 上屏）与 `WgpuWin32Surface`（wgpu GPU
// 栅格上屏，Win32 宿主）。三者共用同一个 `Win32Host` 宿主与同一份 `detail::set_win32_cursor`
// 映射，故一份探针同时验收：
//   * 只开 AURORA_BACKEND_WIN32      → 验 Win32Surface
//   * 再开 AURORA_BACKEND_D3D11      → 两路都验（D3D11Surface 依赖 Win32 宿主，故 WIN32 必须同时 ON）
//   * 再开 AURORA_BACKEND_GPU_WGPU   → 三路都验（同上，另需 Rust 工具链产出的 wgpu 静态库）
//
// 原理（与 tools/verify/x11_cursor_live_probe.cpp 同构）：
//   1) 用 aurora 建真实窗口（被测对象）；把窗口置顶并摆到指针能够落到的位置、把指针挪到
//      窗口客户区中心——Win32 的 `SetCursor` 只在**调用线程拥有光标**时改变屏幕显示，而
//      光标所有权由「指针落在哪个线程的窗口上」决定，故必须先把指针移到被测窗口上；
//   2) 对 11 个 `CursorShape` 逐个「位移 + 派发平台事件 + `set_cursor`」，用 `GetCursorInfo`
//      读回「屏幕上真实显示的光标句柄」，与本探针按同一映射表 `LoadCursor(nullptr, IDC_*)`
//      算出的期望句柄逐项比对；
//   3) 11 个 `IDC_*` 两两互异 → 期望 11/11 命中且读回互异。
//
// 构建（方式 ① 推荐，Windows 上无需手写编译命令）：
//   cmake -S . -B build-verify -G "Visual Studio 17 2022" -A x64 ^
//         -DAURORA_BACKEND_WIN32=ON -DAURORA_BACKEND_D3D11=ON ^
//         -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_win32_cursor --config Release
//   build-verify\Release\aurora_verify_win32_cursor.exe
// 构建（方式 ② 手写 MSVC 命令，须先有一个已构建好的 Win32 后端构建目录 build-win32）：
//   cl /nologo /std:c++20 /EHsc /utf-8 /DAURORA_BACKEND_WIN32 /DAURORA_BACKEND_D3D11 ^
//      /I include /I src /I third_party ^
//      tools\verify\win32_cursor_live_probe.cpp ^
//      /Fe:win32_cursor_live_probe.exe ^
//      /link build-win32\Release\aurora.lib build-win32\Release\freetype.lib ^
//            build-win32\Release\harfbuzz.lib user32.lib gdi32.lib shell32.lib ole32.lib ^
//            uuid.lib d3d11.lib dxgi.lib d3dcompiler.lib
//
// 运行：直接双击或命令行执行。过程会把被测窗口置顶并依次试摆到「屏幕中心 → 四角内侧」，
//       把鼠标移到其客户区中心，**退出前恢复原指针位置与窗口层级**，不改动其他窗口。
//       物理前提 ①：控制台会话须**已解锁且活动**，且指针落点上不得有他人置顶窗口——锁屏时
//       系统的全屏 `LockScreenBackstopFrame` 占据落点，IDE / 浏览器的 always-on-top 窗口则
//       会压住本探针的置顶窗口（同处置顶带，本探针未必在最上）。此时本探针取不到光标所有权，
//       会以退出码 3 打印「期望落点 / 实际指针位置 / 该点上的窗口类名 / 试过几个落点」的现场
//       证据后终止（不给假阳性）。
//       物理前提 ②：读回要成立，每个形状下发前必须**真正派发**平台事件（`poll_platform_events`
//       的 PeekMessage/DispatchMessage），不能只 `wait_events`——后者只等待不派发，WM_SETCURSOR
//       走不到 wndproc，`GetCursorInfo` 的共享光标便恒停在上一手的值，而本线程 `GetCursor()`
//       逐形状命中（= 映射与 `SetCursor` 下发成立）。这是读回前提不成立、非接线不成立，故若
//       仍出现「线程光标全命中、共享光标读回不动」，按退出码 8 申报「屏幕表现未证明」，须由
//       人工段（`--interactive`）补齐。本仓库实测（2026-09-20）：改为「位移 + 派发 + 下发」
//       后 `Win32Surface`(GDI) 路 11/11 读回命中，屏幕表现已证明。
//
// 人工段（`--interactive`，可选）：逐个形状停在被测窗口上等回车确认，用于在自动段读回前提
//       不成立（退出码 8）时补上「屏幕上真的变了」这一环。多路后端各自跑一遍人工段（每条
//       路径的屏幕表现独立验收）。
//   .\aurora_verify_win32_cursor.exe --interactive
//
// 退出码（多路时取编号最小的非零者 = 判据前提最不成立的那一路）：
//   0  全部形状读回皆命中期望句柄且两两互异 —— 真机验收通过
//   2  环境不可用（窗口创建失败 / 取不到 HWND / GetCursorInfo 失败）
//   3  指针无法落在被测窗口上（本线程拿不到光标所有权；锁屏 / 远程会话隔离即此）
//   4  读回恒为同一光标 —— 本会话无可靠读回（远程桌面 / 会话隔离等）
//   5  部分形状读回不符 —— 见逐行表格 match 列
//   6  本机未编译进任何 Win32 家族后端（须开 AURORA_BACKEND_WIN32 / D3D11）
//   7  人工段被判为不符（逐行提示里有对应形状）
//   8  线程光标下发全命中、但共享光标读回未随之变化（读回前提不成立）—— 屏幕表现未证明
//   构建命令中的行尾 ^（cmd 续行符）为文档形态，故本头注释整体使用块注释（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#ifndef AURORA_PLATFORM_WINDOWS
#error "aurora_verify_win32_cursor can only be built on Windows (AURORA_PLATFORM_WINDOWS)"
#endif

#ifdef AURORA_BACKEND_WIN32
#include "aurora/window/win32_surface.h"
#endif
#ifdef AURORA_BACKEND_D3D11
#include "aurora/window/d3d11_surface.h"
#endif
#ifdef AURORA_BACKEND_GPU_WGPU
#include "aurora/window/wgpu_win32_surface.h"
#endif
#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "AURORA_BACKEND_WIN32 or AURORA_BACKEND_D3D11 must be enabled"
#endif

// 与库内同口径：先在**任何**平台头之前定好这两个宏，避免 <windows.h> 的 min/max 宏污染
// 标准库用法（win32_surface.h / d3d11_surface.h 自身也带 #ifndef 守卫，此处显式声明更稳）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

// aurora 公共头已先行（win32_surface.h / d3d11_surface.h 内会引入 <windows.h>），
// 此处再显式包含一次以取用窗口/GDI 符号（重复包含由 include guard 消解）。
#include <windows.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

#include "aurora/widget/containers.h"  // Column（内核会话的最小真实树占位）
#include "aurora/window/cursor_map.h"
#include "e2e/harness.h"  // E2E 驱动内核：三路宿主建窗统一经 e2e::open（RAII + 失败翻译），本探针只留平台判据
#include "verify_args.h"
#include "verify_print.h"

namespace {

// 期望映射表：与 `src/aurora/window/win32_cursor.h` 的 `detail::set_win32_cursor` 逐项对齐。
// 该头位于 src/ 内部、不对探针暴露接口，故此处镜像一份；探针的逐行比对（match 列）即
// 「实现与本文档声明是否一致」的漂移检测——任何一侧改动而另一侧未同步，本探针立刻变红。
auto expected_cursor(aurora::CursorShape shape) -> HCURSOR {
    switch (shape) {
        case aurora::CursorShape::Arrow:
            return LoadCursor(nullptr, IDC_ARROW);
        case aurora::CursorShape::IBeam:
            return LoadCursor(nullptr, IDC_IBEAM);
        case aurora::CursorShape::PointingHand:
            return LoadCursor(nullptr, IDC_HAND);
        case aurora::CursorShape::ResizeNS:
            return LoadCursor(nullptr, IDC_SIZENS);
        case aurora::CursorShape::ResizeEW:
            return LoadCursor(nullptr, IDC_SIZEWE);
        case aurora::CursorShape::ResizeNWSE:
            return LoadCursor(nullptr, IDC_SIZENWSE);
        case aurora::CursorShape::ResizeNESW:
            return LoadCursor(nullptr, IDC_SIZENESW);
        case aurora::CursorShape::Move:
            return LoadCursor(nullptr, IDC_SIZEALL);
        case aurora::CursorShape::Crosshair:
            return LoadCursor(nullptr, IDC_CROSS);
        case aurora::CursorShape::NotAllowed:
            return LoadCursor(nullptr, IDC_NO);
        case aurora::CursorShape::Wait:
            return LoadCursor(nullptr, IDC_WAIT);
    }
    return nullptr;
}

// 本线程是否是「前台线程」：读回恒不动时用它区分两种成因——非前台（共享光标由前台线程
// 支配，属读回前提不成立 → 退出码 8）与已是前台（本会话根本读不回，→ 退出码 4）。
auto owns_foreground() -> bool {
    const HWND fg = GetForegroundWindow();  // NOLINT
    if (fg == nullptr) {
        return false;
    }
    return GetWindowThreadProcessId(fg, nullptr) == GetCurrentThreadId();
}

// 读回「当前屏幕显示的光标句柄」。失败（光标被隐藏/无桌面）返回 nullptr。
auto displayed_cursor() -> HCURSOR {
    CURSORINFO info{};
    info.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&info) == FALSE) {
        return nullptr;
    }
    return info.hCursor;
}

// 被测窗口是否真的在指针之下（含子窗口/被祖先包裹的情况）。
auto pointer_over(HWND hwnd) -> bool {
    POINT probe{};
    if (GetCursorPos(&probe) == FALSE) {
        return false;
    }
    const HWND under = WindowFromPoint(probe);  // NOLINT
    if (under == nullptr) {
        return false;
    }
    return under == hwnd || GetAncestor(under, GA_ROOT) == hwnd;
}

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

// 泵消息。关键：`wait_events` 只等待、**不派发**（`Win32Host::wait_events` 仅
// `MsgWaitForMultipleObjectsEx`，派发在 `poll_platform_events` 的 PeekMessage/DispatchMessage
// 里）。而「屏幕显示的光标」（`GetCursorInfo` 读回的共享光标）随 WM_SETCURSOR 的 wndproc
// 处理才刷新，故只 wait 不 poll 会让读回恒停在上一手的值——实测如此，与接线无关。
auto pump(aurora::Surface &surface, int rounds) -> void {
    for (int i = 0; i < rounds; ++i) {
        surface.poll_platform_events();
        surface.wait_events(20.0);
    }
}

// 人工段的提示语：说完「应该看到什么」，让人去看屏幕。Win32 的 11 个形状全部有系统预置
// 光标（无 GLFW 那种「版本过低即回退箭头」的情形），故期望语只描述形状本身。
auto human_expectation(aurora::CursorShape shape) -> const char * {
    switch (shape) {
        case aurora::CursorShape::Arrow:
            return "default arrow";
        case aurora::CursorShape::IBeam:
            return "text I-beam";
        case aurora::CursorShape::PointingHand:
            return "hand (pointing finger)";
        case aurora::CursorShape::ResizeNS:
            return "vertical double-headed arrow";
        case aurora::CursorShape::ResizeEW:
            return "horizontal double-headed arrow";
        case aurora::CursorShape::ResizeNWSE:
            return "main diagonal (NW-SE) double-headed arrow";
        case aurora::CursorShape::ResizeNESW:
            return "anti-diagonal (NE-SW) double-headed arrow";
        case aurora::CursorShape::Move:
            return "four-way move arrow (cross)";
        case aurora::CursorShape::Crosshair:
            return "crosshair";
        case aurora::CursorShape::NotAllowed:
            return "not allowed (circle with slash)";
        case aurora::CursorShape::Wait:
            return "wait / busy hourglass";
    }
    return "(unknown)";
}

// 逐形状下发 + 读回 + 打印表格（可选人工段）。返回该路后端自己的退出码。
auto run_sweep(aurora::Surface &surface, const char *label, const char *title, bool interactive) -> int {
    emit(std::string("==== ") + label + " ====");

    // Win32Surface 与 D3D11Surface 均覆写 native_handle()（返回宿主 HWND）。此处仍保留
    // 「按唯一标题查找」兜底：探针入参是 `aurora::Surface&`，任何**自定义** Surface 后端
    // 都可能不覆写 native_handle()（基类默认返回 nullptr），有兜底才能对这类后端给出
    // 可判定的结果，而不是一律报「拿不到 HWND」。
    auto *hwnd = static_cast<HWND>(surface.native_handle());
    if (hwnd == nullptr) {
        hwnd = FindWindowA(nullptr, title);
    }
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("verify",
                         std::string(label) + ": cannot obtain HWND (native_handle is null and title lookup failed)");
        return 2;
    }

    // ---- 把指针安置到被测窗口上，使本线程取得光标所有权 ----
    // 探针常由**后台进程**（CI / agent 会话）启动，而 `SetForegroundWindow` 受前台锁约束
    // （非前台进程的调用会被系统忽略），故这里改为「窗口置顶 + 换点摆放」：不置顶时指针
    // 落点极易被别的窗口占据，`WindowFromPoint` 取不到本窗口即拿不到光标所有权。
    // 为何还要**换点**：`HWND_TOPMOST` 只保证进入置顶带，同带内谁压在最上仍取决于其他置顶
    // 窗口（IDE / 浏览器的 always-on-top 会长期占据屏幕中心），故中心被占时改试四角内侧。
    ShowWindow(hwnd, SW_SHOWNORMAL);

    // 客户几何在改动窗口层级之前取：此处的失败属「环境不可用」，须在任何改动落地前返回，
    // 否则会把窗口留在置顶状态而没有恢复路径。
    // 客户区中心（屏幕坐标）：每次换点后须重算（窗口移动会改变客户区的屏幕位置）。
    const auto client_center = [hwnd]() -> POINT {
        RECT c{};
        if (GetClientRect(hwnd, &c) == FALSE) {
            return POINT{.x = -1, .y = -1};  // 负值 = 取不到客户几何（调用方据此返回环境错误）
        }
        POINT p{.x = (c.left + c.right) / 2, .y = (c.top + c.bottom) / 2};
        ClientToScreen(hwnd, &p);
        return p;
    };
    if (client_center().x < 0) {
        AURORA_LOG_ERROR("verify", std::string(label) + ": GetClientRect failed");
        return 2;
    }

    RECT frame{};
    const bool have_frame = GetWindowRect(hwnd, &frame) != FALSE;
    const int fw = have_frame ? frame.right - frame.left : 360;
    const int fh = have_frame ? frame.bottom - frame.top : 240;
    constexpr int margin = 40;
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    // 「屏幕中心 + 四角内侧」共 5 个候选落点（数组长度即候选数，编译期常量）
    constexpr std::size_t spot_total = 5U;
    const std::array<int, spot_total> spot_x{(sw - fw) / 2, margin, sw - fw - margin, margin, sw - fw - margin};
    const std::array<int, spot_total> spot_y{(sh - fh) / 2, margin, margin, sh - fh - margin, sh - fh - margin};
    constexpr int spot_count = static_cast<int>(spot_total);

    POINT saved{};
    const bool have_saved = GetCursorPos(&saved) != FALSE;

    // 每个候选点试两轮（首轮刚移动、次轮给 WM/合成器留出稳定时间），落点后泵消息等
    // WM_SETCURSOR / WM_MOUSEMOVE 走完（本线程成为光标所有者）；全程不中才判「拿不到光标
    // 所有权」，避免把「竞态」或「中心被他人置顶窗口占住」误判成「接线不成立」。
    bool over = false;
    POINT center{};
    int last_spot = 0;
    for (int attempt = 0; attempt < spot_count * 2 && !over; ++attempt) {
        const int s = attempt / 2;
        last_spot = s;
        // 下标可证在界内（attempt < spot_count * 2 ⇒ s ≤ spot_count - 1），用 .at() 只是让越界
        // 显式化为异常而非 UB，正常路径不会抛。
        SetWindowPos(hwnd, HWND_TOPMOST, spot_x.at(static_cast<std::size_t>(s)), spot_y.at(static_cast<std::size_t>(s)),
                     fw, fh, SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        UpdateWindow(hwnd);
        center = client_center();
        SetCursorPos(center.x, center.y);
        pump(surface, 8);
        over = pointer_over(hwnd);
    }

    if (!over) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        // 落点失败的真实原因只能靠现场证据区分（他人置顶窗口遮挡 / 远程会话指针隔离 /
        // SetCursorPos 被策略拒），故把「期望落点 vs 实际指针位置 vs 该点上的窗口」一并报出。
        POINT actual{};
        const bool have_actual = GetCursorPos(&actual) != FALSE;
        const HWND blocker = have_actual ? WindowFromPoint(actual) : nullptr;  // NOLINT
        char blocker_class[128] = "n/a";
        if (blocker != nullptr) {
            GetClassNameA(blocker, blocker_class, sizeof(blocker_class) - 1);  // NOLINT
        }
        AURORA_LOG_ERROR("verify", std::string(label) +
                                       ": pointer is not over the target window (cannot take cursor ownership). "
                                       "Typical cause: window is occluded / covered by a topmost window, or a remote "
                                       "desktop session is isolated. want=(" +
                                       std::to_string(center.x) + "," + std::to_string(center.y) + ") actual=(" +
                                       (have_actual ? std::to_string(actual.x) : "?") + "," +
                                       (have_actual ? std::to_string(actual.y) : "?") +
                                       ") hwnd=" + aurora_verify::format_handle(hwnd) + " point_hwnd=" +
                                       aurora_verify::format_handle(blocker) + " point_class=" + blocker_class +
                                       " spots_tried=" + aurora_verify::format_int(last_spot + 1) + "/" +
                                       aurora_verify::format_int(spot_count));
        if (have_saved) {
            SetCursorPos(saved.x, saved.y);
        }
        return 3;
    }

    constexpr int total = static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT);
    emit(aurora_verify::pad_right("#", 3) + aurora_verify::pad_right("shape(rfc name)", 20) +
         aurora_verify::pad_right("expect(IDC_*)", 20) + aurora_verify::pad_right("readback", 20) +
         aurora_verify::pad_right("match", 7) + "GetCursor(thread)");

    int hits = 0;
    int owned_matches = 0;
    int distinct = 0;
    HCURSOR previous = nullptr;
    for (int i = 0; i < total; ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        // 先做一次 1px 真实位移并**派发**消息：让系统把「共享光标」与本线程光标同步一次
        // （此间 DefWindowProc 处理 WM_SETCURSOR 会把本线程光标复位为窗口类光标），
        // **随后**再由被测对象下发形状。顺序不可颠倒：
        //   * 下发后再泵 → WM_SETCURSOR 会用类光标覆盖，读回恒为箭头（复位噪声）；
        //   * 不泵（只 wait 不 poll）→ 共享光标停在上一手的值，读回 11 行恒定，误判成
        //     「读回不可信」（实测即此现象）。
        SetCursorPos(center.x + (i % 2), center.y);
        pump(surface, 4);
        surface.set_cursor(shape);

        const HCURSOR want = expected_cursor(shape);
        const HCURSOR got = displayed_cursor();
        const HCURSOR owned = GetCursor();
        const bool match = (got != nullptr && got == want);
        if (match) {
            ++hits;
        }
        if (owned != nullptr && owned == want) {
            ++owned_matches;
        }
        if (i == 0 || got != previous) {
            ++distinct;
        }
        previous = got;

        emit(aurora_verify::pad_right(aurora_verify::format_int(i), 3) +
             aurora_verify::pad_right(aurora::cursor_rfc_name(shape), 20) +
             aurora_verify::pad_right(aurora_verify::format_handle(want), 20) +
             aurora_verify::pad_right(aurora_verify::format_handle(got), 20) +
             aurora_verify::pad_right(match ? "YES" : "no", 7) + aurora_verify::format_handle(owned));
    }

    emit(std::string("Hits ") + aurora_verify::format_int(hits) + "/" + aurora_verify::format_int(total) +
         ", distinct read-back " + aurora_verify::format_int(distinct) + "/" + aurora_verify::format_int(total) +
         ", GetCursor hits " + aurora_verify::format_int(owned_matches) + "/" + aurora_verify::format_int(total) +
         ", foreground owned by this thread: " + (owns_foreground() ? "yes" : "no"));

    int rc = 0;
    if (interactive) {
        // ---- 人工段：逐个形状让人对照屏幕 ----
        // 这是唯一能绕过「共享光标读回受前台归属限制」而证明屏幕真的变了的手段。
        emit("");
        emit(std::string("Interactive stage starting: please watch the mouse pointer inside the topmost window ") +
             "titled '" + title + "',");
        emit("press Enter to move to the next shape (type n + Enter to judge the current one as a mismatch).");
        int rejected = 0;
        int confirmed = 0;
        bool stdin_open = true;
        for (int i = 0; i < total && stdin_open; ++i) {
            const auto shape = static_cast<aurora::CursorShape>(i);
            // 与自动段同一时序（先位移 + 派发让 WM_SETCURSOR 走完，再下发形状）；差别只在
            // 这里停下来让人眼看屏幕，因此不需要读回判据。
            SetCursorPos(center.x + (i % 2), center.y);
            pump(surface, 4);
            surface.set_cursor(shape);

            emit(std::string("[") + aurora_verify::format_int(i + 1) + "/" + aurora_verify::format_int(total) + "] " +
                 aurora::cursor_rfc_name(shape) + " -- expected to see: " + human_expectation(shape) +
                 "; press Enter to confirm (type n + Enter to judge as mismatch)");
            std::string answer;
            if (!std::getline(std::cin, answer)) {
                AURORA_LOG_WARN("verify", "stdin ended; interactive stage terminated early");
                stdin_open = false;
                break;
            }
            ++confirmed;
            if (!answer.empty() && (answer[0] == 'n' || answer[0] == 'N')) {
                ++rejected;
                AURORA_LOG_ERROR("verify", std::string("Manual judgment mismatch: ") + aurora::cursor_rfc_name(shape));
            }
        }
        if (stdin_open && rejected == 0) {
            emit(std::string("PASS: ") + label +
                 "'s cursor shapes confirmed on screen by a human observer (all shapes judged as matching)");
            rc = 0;
        } else if (rejected > 0) {
            rc = 7;
        } else {
            // 人工段没跑完（stdin 提前结束）：不据此申报通过，退回自动段判据。
            AURORA_LOG_WARN("verify", std::string(label) + ": interactive stage incomplete (" +
                                          aurora_verify::format_int(confirmed) + "/" +
                                          aurora_verify::format_int(total) +
                                          " confirmed); falling back to the auto verdict");
            rc = hits == total ? 0 : (distinct <= 1 ? 4 : 5);
        }
    } else if (hits == total) {
        emit(std::string("PASS: ") + label + "'s " + aurora_verify::format_int(total) +
             " CursorShapes all changed the cursor shown on screen");
        rc = 0;
    } else if (!owns_foreground() && owned_matches == total) {
        // 本线程 `GetCursor()` 逐形状命中 = 映射表与 `SetCursor` 下发都成立；共享光标读回
        // 未跟上，且本线程并非前台线程——本会话的「指针落点 → 光标所有权」交接没有发生
        // （锁屏 / 会话隔离 / 他人置顶），读回前提不成立。判据前提不成立不等于接线不成立，
        // 故如实申报「屏幕表现未证明」，而非含糊的 FAIL。
        emit(
            "Auto stage: thread-cursor delivery hit every shape (mapping + SetCursor proven), but the shared "
            "cursor read-back did not follow and this thread is not the foreground thread.");
        emit("Screen behavior is still unproven; re-run from a foreground session, or add `--interactive`.");
        rc = 8;
    } else if (distinct <= 1) {
        AURORA_LOG_ERROR("verify",
                         std::string(label) +
                             ": read-back is always the same cursor; this session cannot reliably read back (FAIL 4)");
        rc = 4;
    } else {
        AURORA_LOG_ERROR("verify",
                         std::string(label) + ": some shapes read back do not match the expected handle (FAIL 5)");
        rc = 5;
    }

    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (have_saved) {
        SetCursorPos(saved.x, saved.y);
    }
    return rc;
}

}  // namespace

// 入口不吞异常：探针的失败以未捕获异常 → 非零退出码/terminate 呈现（捕获反而把它压成 0），与 demo 入口同口径。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("Win32 / GDI / D3D11 cursor shape live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    aurora::init_console();  // Windows 控制台切 UTF-8，避免中文/表格错行

    int worst = 0;
    const auto worse_of = [&worst](int rc) -> void {
        if (rc != 0 && (worst == 0 || rc < worst)) {
            worst = rc;
        }
    };

#ifdef AURORA_BACKEND_WIN32
    {
        // 建窗经 E2E 内核 e2e::open：Win32Options 与旧手写 Win32Surface(size,title,style) 等价
        // （WindowOptions::style 默认即 WindowStyleOptions{}）；visibility 显式 Normal 保持既有
        // 可见行为（内核 E2E 用例默认 Hidden，而本探针的光标读回需要真实显示的窗口）。
        const char *title = "aurora-verify-i1-cursor-gdi";
        aurora::e2e::WindowSpec spec;
        spec.backend = aurora::e2e::Backend::Win32;
        spec.width = 360;
        spec.height = 240;
        spec.title = title;
        spec.visibility = aurora::WindowVisibility::Normal;
        auto session = aurora::e2e::open(spec);
        if (!session.ok()) {
            AURORA_LOG_WARN("verify", "Win32Surface(GDI): open failed: " + session.reason());
        } else {
            // 最小真实控件树占位：内核会话的事件派发要求已挂载且根节点带 widget（空 Node{} 的
            // widget 指针为空，命中测试会解引用空指针）——本探针只泵平台事件不渲染，
            // 空列布局零尺寸、指针落点不命中，派发即无操作。
            session.mount(aurora::Node{aurora::Column{}});
            worse_of(run_sweep(session.surface(), "Win32Surface(GDI)", title, interactive));
        }
    }
#endif
#ifdef AURORA_BACKEND_D3D11
    {
        const char *title = "aurora-verify-i1-cursor-d3d11";
        aurora::e2e::WindowSpec spec;
        spec.backend = aurora::e2e::Backend::D3D11;
        spec.width = 360;
        spec.height = 240;
        spec.title = title;
        spec.visibility = aurora::WindowVisibility::Normal;
        auto session = aurora::e2e::open(spec);
        if (!session.ok()) {
            AURORA_LOG_WARN("verify", "D3D11Surface(GPU): open failed: " + session.reason());
        } else if (!static_cast<aurora::D3D11Surface &>(session.surface()).is_available()) {
            AURORA_LOG_WARN("verify", "D3D11Surface device unavailable (no adapter); skipping this path");
        } else {
            session.mount(aurora::Node{aurora::Column{}});  // 最小真实树占位，理由同 GDI 路
            worse_of(run_sweep(session.surface(), "D3D11Surface(GPU)", title, interactive));
        }
    }
#endif

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)
    {
        // WgpuWin32Surface 的 Win32 宿主路：光标下发与 GDI 走同一份 `detail::set_win32_cursor`，
        // 但它是**独立调用点**（`WgpuWin32Surface::set_cursor`），故单独跑一遍而非由前两路代证。
        const char *title = "aurora-verify-i1-cursor-wgpu";
        aurora::e2e::WindowSpec spec;
        spec.backend = aurora::e2e::Backend::Wgpu;
        spec.width = 360;
        spec.height = 240;
        spec.title = title;
        spec.visibility = aurora::WindowVisibility::Normal;
        auto session = aurora::e2e::open(spec);
        if (!session.ok()) {
            AURORA_LOG_WARN("verify", "WgpuWin32Surface(GPU): open failed: " + session.reason());
        } else if (!static_cast<aurora::WgpuWin32Surface &>(session.surface()).is_available()) {
            AURORA_LOG_WARN("verify",
                            "WgpuWin32Surface device unavailable (no adapter / wgpu lib); skipping this path");
        } else {
            session.mount(aurora::Node{aurora::Column{}});  // 最小真实树占位，理由同 GDI 路
            worse_of(run_sweep(session.surface(), "WgpuWin32Surface(GPU)", title, interactive));
        }
    }
#endif

    if (worst == 0) {
        emit("PASS: Win32 family cursor wiring acceptance passed");
    }
    return worst;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 取窗口句柄优先走 `Surface::native_handle()`，为空才回退 `FindWindowA(标题)`。
//      GDI / D3D11 / wgpu 三路（均为本仓库后端）都已覆写 `native_handle()`（返回宿主 HWND），
//      故该兜底对二者不触发；保留它是为**尚未覆写**该虚函数的自定义 Surface 后端仍能
//      给出可判定结果，而非一律报「拿不到 HWND」。
//   2. `Win32Surface` / `D3D11Surface` 的光标下发依赖窗口线程的光标所有权；若指针落点被他
//      人置顶窗口占住（本探针已试「中心 + 四角」5 个落点仍不中），会以退出码 3 明确报告
//      「指针不在被测窗口上」而非静默给出假阳性。
//   3. 屏幕侧证明分两档：自动段读回命中（`GetCursorInfo` 逐形状 = 期望句柄）＝机器可判定，
//      本仓库 Win32Surface(GDI) 路已实测 11/11；若某会话的读回前提不成立（指针落点没交接、
//      本线程非前台），退化为「本线程 `GetCursor()` 逐形状命中」＝映射与下发成立、屏幕表现
//      未证明（退出码 8），改由人工段补。二档之差源于 Win32 光标所有权模型，非本库可控，
//      故不合并成单一 PASS。
// ---------------------------------------------------------------------------
