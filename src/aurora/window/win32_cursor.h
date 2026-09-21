#pragma once

// Win32 家族共享的光标下发：`Win32Surface`（GDI 上屏）与 `D3D11Surface`（GPU 上屏）
// 共用同一 `Win32Host` 宿主模型，故「语义形状 → 系统预置光标」的映射只应存在一份。
//
// 为何是内部头（src/）而非公共头：`win32_host.h` 刻意 pimpl 隔离、公共头不含 <windows.h>，
// 而本文件需要 `SetCursor`/`LoadCursor` 与 `IDC_*`，故与 `win32_capture.h` / `swizzle.h` /
// `keysym_map.h` 同列于 src/aurora/window/，由后端 .cpp 以 "aurora/window/win32_cursor.h" 引入，
// 不进公共 API 面（亦无 codespec / 测试目标单元之义务）。
#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#include "aurora/core/enums.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif
#include <windows.h>

namespace aurora::detail {

/// @brief 把语义光标形状下发为 Win32 系统预置光标（`SetCursor` + `LoadCursor(nullptr, IDC_*)`）。
///
/// 映射：Arrow→`IDC_ARROW`、IBeam→`IDC_IBEAM`、PointingHand→`IDC_HAND`、ResizeNS→`IDC_SIZENS`、
/// ResizeEW→`IDC_SIZEWE`、ResizeNWSE→`IDC_SIZENWSE`、ResizeNESW→`IDC_SIZENESW`、Move→`IDC_SIZEALL`、
/// Crosshair→`IDC_CROSS`、NotAllowed→`IDC_NO`、Wait→`IDC_WAIT`。
///
/// 两处关键细节：
/// ① 用**泛型宏** `SetCursor` / `LoadCursor` 而非显式 `…W`——`IDC_*` 由 `MAKEINTRESOURCE` 定义且
///    不带 A/W 后缀，泛型宏才随 `UNICODE` 定义解析到同一字符域。本项目 CMake **未定义 `UNICODE`**，
///    写死 `LoadCursorW` 会在 ANSI 构建下因 `IDC_ARROW` 为 `LPSTR` 而类型不匹配。
/// ② 系统预置光标由 OS 拥有，**不需要**释放，故无资源泄漏，也不必像 X11/GLFW 那样缓存句柄。
///
/// @note Thread: 作用于调用线程的窗口光标（Win32 `SetCursor` 契约）；由事件派发栈（主线程）调用。
/// @note 未在本仓库无头构建内编译验证（须 Win32/D3D11 后端构建后复查）。
inline auto set_win32_cursor(CursorShape shape) -> void {
    switch (shape) {
        case CursorShape::Arrow:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            break;
        case CursorShape::IBeam:
            SetCursor(LoadCursor(nullptr, IDC_IBEAM));
            break;
        case CursorShape::PointingHand:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            break;
        case CursorShape::ResizeNS:
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            break;
        case CursorShape::ResizeEW:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            break;
        case CursorShape::ResizeNWSE:
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            break;
        case CursorShape::ResizeNESW:
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
            break;
        case CursorShape::Move:
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            break;
        case CursorShape::Crosshair:
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            break;
        case CursorShape::NotAllowed:
            SetCursor(LoadCursor(nullptr, IDC_NO));
            break;
        case CursorShape::Wait:
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            break;
    }
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
