/* 光标形状 —— Win32 家族真机验收探针（人工触发的验收工具，不进 CTest）
// ============================================================================
// 覆盖后端：`Win32Surface`（GDI 上屏）与 `D3D11Surface`（GPU 上屏）。两者共用同一个
// `Win32Window` 宿主与同一份 `detail::set_win32_cursor` 映射，故一份探针同时验收：
//   * 只开 AURORA_BACKEND_WIN32      → 验 Win32Surface
//   * 再开 AURORA_BACKEND_D3D11      → 两路都验（D3D11Surface 依赖 Win32 宿主，故 WIN32 必须同时 ON）
//
// 原理（与 tools/verify/x11_cursor_live_probe.cpp 同构）：
//   1) 用 aurora 建真实窗口（被测对象）；把窗口置前、把指针挪到窗口客户区中心——
//      Win32 的 `SetCursor` 只在**调用线程拥有光标**时改变屏幕显示，而光标所有权由
//      「指针落在哪个线程的窗口上」决定，故必须先把指针移到被测窗口上；
//   2) 对 11 个 `CursorShape` 逐个 `set_cursor`，用 `GetCursorInfo` 读回「屏幕上真实
//      显示的光标句柄」，与本探针按同一映射表 `LoadCursor(nullptr, IDC_*)` 算出的期望
//      句柄逐项比对；
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
// 运行：直接双击或命令行执行。过程会短暂把鼠标移到探针窗口中心，**退出前恢复原指针
//       位置**，不改动其他窗口。
//
// 退出码（多路时取其中最差者）：
//   0  全部形状读回皆命中期望句柄且两两互异 —— 真机验收通过
//   2  环境不可用（窗口创建失败 / 取不到 HWND / GetCursorInfo 失败）
//   3  指针无法落在被测窗口上（本线程拿不到光标所有权）
//   4  读回恒为同一光标 —— 本会话无可靠读回（远程桌面 / 会话隔离等）
//   5  部分形状读回不符 —— 见逐行表格 match 列
//   6  本机未编译进任何 Win32 家族后端（须开 AURORA_BACKEND_WIN32 / D3D11）
//   构建命令中的行尾 ^（cmd 续行符）为文档形态，故本头注释整体使用块注释（避免 -Wcomment）。
// ============================================================================ */

#include "aurora/core/log.h"
#include "aurora/core/platform.h"

#if !defined(AURORA_PLATFORM_WINDOWS)
#error "aurora_verify_win32_cursor 只能在 Windows 上构建（AURORA_PLATFORM_WINDOWS）"
#endif

#if defined(AURORA_BACKEND_WIN32)
#include "aurora/window/win32_surface.h"
#endif
#if defined(AURORA_BACKEND_D3D11)
#include "aurora/window/d3d11_surface.h"
#endif
#if !defined(AURORA_BACKEND_WIN32) && !defined(AURORA_BACKEND_D3D11)
#error "须开启 AURORA_BACKEND_WIN32 或 AURORA_BACKEND_D3D11"
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

#include <cstdint>
#include <string>

#include "aurora/window/cursor_map.h"
#include "verify_print.h"

namespace {

/// 期望映射表：与 `src/aurora/window/win32_cursor.h` 的 `detail::set_win32_cursor` 逐项对齐。
/// 该头位于 src/ 内部、不对探针暴露接口，故此处镜像一份；探针的逐行比对（match 列）即
/// 「实现与本文档声明是否一致」的漂移检测——任何一侧改动而另一侧未同步，本探针立刻变红。
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

/// 读回「当前屏幕显示的光标句柄」。失败（光标被隐藏/无桌面）返回 nullptr。
auto displayed_cursor() -> HCURSOR {
    CURSORINFO info{};
    info.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&info) == FALSE) {
        return nullptr;
    }
    return info.hCursor;
}

/// 被测窗口是否真的在指针之下（含子窗口/被祖先包裹的情况）。
auto pointer_over(HWND hwnd) -> bool {
    POINT probe{};
    if (GetCursorPos(&probe) == FALSE) {
        return false;
    }
    const HWND under = WindowFromPoint(probe);
    if (under == nullptr) {
        return false;
    }
    return under == hwnd || GetAncestor(under, GA_ROOT) == hwnd;
}

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// 逐形状下发 + 读回 + 打印表格。返回该路后端自己的退出码。
auto run_sweep(aurora::Surface &surface, const char *label, const char *title) -> int {
    emit(std::string("==== ") + label + " ====");

    // Win32Surface 与 D3D11Surface 均覆写 native_handle()（返回宿主 HWND）。此处仍保留
    // 「按唯一标题查找」兜底：探针入参是 `aurora::Surface&`，任何**自定义** Surface 后端
    // 都可能不覆写 native_handle()（基类默认返回 nullptr），有兜底才能对这类后端给出
    // 可判定的结果，而不是一律报「拿不到 HWND」。
    HWND hwnd = static_cast<HWND>(surface.native_handle());
    if (hwnd == nullptr) {
        hwnd = FindWindowA(nullptr, title);
    }
    if (hwnd == nullptr) {
        AURORA_LOG_ERROR("verify", std::string(label) + "：拿不到 HWND（native_handle 为 null 且按标题查找失败）");
        return 2;
    }

    // ---- 把指针安置到被测窗口上，使本线程取得光标所有权 ----
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);

    POINT saved{};
    const bool have_saved = GetCursorPos(&saved) != FALSE;

    RECT client{};
    if (GetClientRect(hwnd, &client) == FALSE) {
        AURORA_LOG_ERROR("verify", std::string(label) + "：GetClientRect 失败");
        return 2;
    }
    POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    ClientToScreen(hwnd, &center);
    SetCursorPos(center.x, center.y);

    // 等待 WM_SETCURSOR / WM_MOUSEMOVE 走完，本线程成为光标所有者。
    for (int i = 0; i < 8; ++i) {
        surface.wait_events(20.0);
    }

    if (!pointer_over(hwnd)) {
        AURORA_LOG_ERROR("verify", std::string(label) +
                                       "：指针不在被测窗口上（拿不到光标所有权）。"
                                       "典型原因：窗口被遮挡/被置顶窗口压住、或远程桌面会话隔离。");
        if (have_saved) {
            SetCursorPos(saved.x, saved.y);
        }
        return 2;
    }

    const int total = static_cast<int>(aurora::kCursorShapeCount);
    emit(aurora_verify::pad_right("#", 3) + aurora_verify::pad_right("shape(rfc name)", 20) +
         aurora_verify::pad_right("expect(IDC_*)", 20) + aurora_verify::pad_right("readback", 20) +
         aurora_verify::pad_right("match", 7) + "GetCursor(thread)");

    int hits = 0;
    int owned_matches = 0;
    int distinct = 0;
    HCURSOR previous = nullptr;
    for (int i = 0; i < total; ++i) {
        const auto shape = static_cast<aurora::CursorShape>(i);
        // 注意：set_cursor 与读回之间**不泵消息**——DEFWNDPROC 处理 WM_SETCURSOR 时会用
        // 窗口类光标覆盖 SetCursor 的结果，泵消息会给「未移动鼠标时的光标」引入复位噪声。
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
             aurora_verify::pad_right(match ? "YES" : "no", 7) +
             aurora_verify::format_handle(owned));
    }

    if (have_saved) {
        SetCursorPos(saved.x, saved.y);
    }

    emit(std::string("命中 ") + aurora_verify::format_int(hits) + "/" + aurora_verify::format_int(total) +
         "，读回互异 " + aurora_verify::format_int(distinct) + "/" + aurora_verify::format_int(total) +
         "，GetCursor 命中 " + aurora_verify::format_int(owned_matches) + "/" + aurora_verify::format_int(total));

    if (distinct <= 1) {
        AURORA_LOG_ERROR("verify", std::string(label) + "：读回恒为同一光标，本会话不支持可靠读回（FAIL 4）");
        return 4;
    }
    if (hits != total) {
        AURORA_LOG_ERROR("verify", std::string(label) + "：部分形状读回与期望句柄不符（FAIL 5）");
        return 5;
    }
    emit(std::string("PASS: ") + label + " 的 11 个 CursorShape 皆改变了屏幕显示的光标");
    return 0;
}

}  // namespace

auto main() -> int {
    aurora::init_console();  // Windows 控制台切 UTF-8，避免中文/表格错行

    int worst = 0;
    const auto worse_of = [&worst](int rc) -> void {
        if (rc != 0 && (worst == 0 || rc < worst)) {
            worst = rc;
        }
    };

#if defined(AURORA_BACKEND_WIN32)
    {
        const char *title = "aurora-verify-i1-cursor-gdi";
        aurora::Win32Surface surface(360, 240, title, aurora::WindowStyleOptions{});
        worse_of(run_sweep(surface, "Win32Surface(GDI)", title));
    }
#endif
#if defined(AURORA_BACKEND_D3D11)
    {
        const char *title = "aurora-verify-i1-cursor-d3d11";
        aurora::D3D11Surface surface(360, 240, title, aurora::WindowStyleOptions{});
        if (!surface.is_available()) {
            AURORA_LOG_WARN("verify", "D3D11Surface 设备不可用（无适配器），跳过该路");
        } else {
            worse_of(run_sweep(surface, "D3D11Surface(GPU)", title));
        }
    }
#endif

    if (worst == 0) {
        emit("PASS: Win32 家族光标接线真机验收通过");
    }
    return worst;
}

// ---------------------------------------------------------------------------
// 已知差距（供后续任务决策，本探针不掩盖）
//   1. 取窗口句柄优先走 `Surface::native_handle()`，为空才回退 `FindWindowA(标题)`。
//      GDI 与 D3D11 两路（均为本仓库后端）都已覆写 `native_handle()`（返回宿主 HWND），
//      故该兜底对二者不触发；保留它是为**尚未覆写**该虚函数的自定义 Surface 后端仍能
//      给出可判定结果，而非一律报「拿不到 HWND」。
//   2. `Win32Surface` / `D3D11Surface` 的光标下发依赖窗口线程的光标所有权；若被测窗口
//      被其他置顶窗口完全遮挡，本探针会以退出码 2 明确报告「指针不在被测窗口上」而非
//      静默给出假阳性。
// ---------------------------------------------------------------------------
