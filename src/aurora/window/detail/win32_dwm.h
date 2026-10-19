#pragma once

// Win32 DWM（Desktop Window Manager）能力探测与帧扩展：**内部头**（与 `win32_dpi.h` 同列于
// src/，不进 include/），仅库实现可见。
//
// 为何单列：CSD 自绘装饰的投影/圆角能力与缩放热区带宽都取决于「DWM composition 是否启用」，
// 而该判定散落在窗口创建/热区计算两处必然漂移。收敛为本头后，`Win32Host` 构造期探测一次、
// 热区带宽经共享层 `csd::frame_margin_dp(frame, capable)` 三路归零，能力与带宽同一真源。
//
// 门控与 `win32_dpi.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

// clang-format off
#include <dwmapi.h>  // DwmIsCompositionEnabled / DwmExtendFrameIntoClientArea（链接 dwmapi）
// clang-format on

namespace aurora::detail {

/// @brief DWM composition 是否启用（投影/圆角/阴影合成的能力判据）。
///
/// 查询失败（老系统无 dwmapi、或 composition 被服务停用）按**不启用**处理——调用方据此把
/// 投影厚度降为 0（`csd::frame_margin_dp` 的 `capable` 形参），不猜「也许有」。
/// @return composition 启用时 true。
[[nodiscard]] inline auto dwm_composition_enabled() -> bool {
    BOOL enabled = FALSE;
    return DwmIsCompositionEnabled(&enabled) == S_OK && enabled != FALSE;
}

/// @brief 把整个客户区标记为 DWM 帧区（投影/圆角的启用入口）。
///
/// 仅在 `dwm_composition_enabled()` 确认后调用（任务书契约）：零边距即「整窗都是帧区」，
/// DWM 据此在窗口外缘绘制投影、在 Win11 上按帧区口径决定圆角。失败（老系统无 dwmapi）
/// 返回 false，调用方静默降级（无投影/圆角），不影响 CSD 其余路径。
/// @param hwnd 目标窗口句柄。
/// @return 调用成功时 true。
[[nodiscard]] inline auto extend_frame_into_client_area(HWND hwnd) -> bool {
    if (hwnd == nullptr) {
        return false;
    }
    const MARGINS margins{.cxLeftWidth = 0, .cxRightWidth = 0, .cyTopHeight = 0, .cyBottomHeight = 0};
    return DwmExtendFrameIntoClientArea(hwnd, &margins) == S_OK;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
