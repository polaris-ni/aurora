#pragma once

// Win32 DPI 取值的**唯一**实现：**内部头**（与 `win32_keymap.h` 同列于 src/，不进 include/），
// 故可被单测直接注入各级解析结果。
//
// 为什么要独立成头而不是留在 `win32_host.cpp` 里：这四层降级链此前**结构上不可达**——
// `GetDpiForWindow` / `GetDpiForSystem` 只在 Win8.1+ / Win10 1607+ 导出，运行时解析而非静态依赖
// SDK 版本宏，而降级判据写的是「函数指针为空」；Win10+ 上 `GetDpiForWindow` 恒已导出，于是
// `GetDpiForSystem` 那一支永远走不到。更要命的是建窗期：`hwnd == nullptr` 时
// `GetDpiForWindow(nullptr)` 返回 **0**（不是失败码），而判据只认「指针为空」，0 被当成有效读数
// 落到 `dpi > 0 ? dpi/96 : 1.0` ⇒ scale 恒 1.0 ⇒ 请求的逻辑 dp 被原样当物理像素消费。
// 收进可单测的内部头后，「取到 0 也算失败」与「逐级可达」由 `utest_win32_dpi` 机械校验。
//
// 门控与 `win32_keymap.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

namespace aurora::detail {

/// @brief 运行时解析到的 DPI API 函数指针集合；全空 = 老系统（逐级回落 `GetDeviceCaps` → 96）。
///
/// 之所以做成可注入的**值**而不是在函数内部直接 `GetProcAddress`：降级链的每一级都需要能被
/// 单测单独证伪（「把 `GetDpiForWindow` 的解析结果人为置空时，读数必须落到 `GetDpiForSystem`
/// 而不是停在 1.0」），把解析与取值分开是拿到这个注入点的最小代价。生产路径走 `resolve_dpi_api()`。
struct DpiApi {
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI *)();
    using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);

    GetDpiForWindowFn for_window = nullptr;  ///< `user32!GetDpiForWindow`（Win10 1607+）。
    GetDpiForSystemFn for_system = nullptr;  ///< `user32!GetDpiForSystem`（Win10 1607+）。
    GetDpiForMonitorFn for_monitor = nullptr;  ///< `shcore!GetDpiForMonitor`（Win8.1+）。
};

/// @brief 一次 DPI 读数的**来源**，供调用方与单测判断降到了哪一级。
enum class DpiSource {
    Window,  ///< `GetDpiForWindow(hwnd)`（句柄就绪后的首选）。
    Monitor,  ///< `GetDpiForMonitor`（建窗期：句柄尚未创建，按落位显示器取）。
    System,  ///< `GetDpiForSystem`（系统级 DPI）。
    DeviceCaps,  ///< `GetDeviceCaps(LOGPIXELSY)`（老系统兜底）。
    Unknown,  ///< 各级均不可得，调用方按 96（scale 1.0）处理。
};

/// @brief 一次 DPI 读数的结果（值 + 来源）。
struct DpiReading {
    int dpi = 0;  ///< 读到的 DPI；0 表示未取到。
    DpiSource source = DpiSource::Unknown;  ///< 该读数来自哪一级。
};

/// @brief 运行时解析 DPI API（生产路径）。
///
/// 四个函数都在 Win8.1 / Win10 1607 之后才导出，故**不能**静态依赖 SDK 版本宏——否则老系统上
/// 整个模块加载失败。解析失败即置空，由 `read_dpi` 逐级回落。
/// @return 解析结果；任一项为 nullptr 表示该级不可用。
[[nodiscard]] inline auto resolve_dpi_api() -> DpiApi {
    DpiApi api;
    // NOLINTBEGIN(*-pro-type-reinterpret-cast, *-casting-through-void)
    const HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 != nullptr) {
        api.for_window = reinterpret_cast<DpiApi::GetDpiForWindowFn>(
            reinterpret_cast<void *>(GetProcAddress(user32, "GetDpiForWindow")));
        api.for_system = reinterpret_cast<DpiApi::GetDpiForSystemFn>(
            reinterpret_cast<void *>(GetProcAddress(user32, "GetDpiForSystem")));
    }
    const HMODULE shcore = GetModuleHandleA("shcore.dll");
    if (shcore != nullptr) {
        api.for_monitor = reinterpret_cast<DpiApi::GetDpiForMonitorFn>(
            reinterpret_cast<void *>(GetProcAddress(shcore, "GetDpiForMonitor")));
    }
    // NOLINTEND(*-pro-type-reinterpret-cast, *-casting-through-void)
    return api;
}

/// @brief 取「窗口即将落位的显示器」（建窗期用）。
///
/// 建窗前没有句柄，唯一可得的落位信息是**主显示器工作区**（`CreateWindowEx` 传 `CW_USEDEFAULT`
/// 时系统就在该工作区内层叠放置）。故首选 `MonitorFromPoint(工作区中心)`；工作区不可得时退回
/// `MONITOR_DEFAULTTOPRIMARY`。多显示器混合 DPI 下这只是**最佳猜测**——窗口真被放到别的屏时，
/// 由宿主在建窗后按 `GetDpiForWindow` 的真实值纠正一次尺寸（见 `win32_host.cpp` 构造体）。
/// @param desired 期望落位的矩形（工作区）；nullptr 表示无信息，按主显示器处理。
/// @return 显示器句柄；不可得时返回 nullptr。
[[nodiscard]] inline auto monitor_for_creation(const RECT *desired) -> HMONITOR {
    POINT pt{};
    if (desired != nullptr) {
        pt.x = desired->left + (desired->right - desired->left) / 2;
        pt.y = desired->top + (desired->bottom - desired->top) / 2;
        return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

/// @brief 按降级链读一次 DPI。**判据是「取到 > 0 才算成功」**，不是「函数指针非空」。
///
/// 链序：`GetDpiForWindow(hwnd)` → `GetDpiForMonitor(落位显示器)` → `GetDpiForSystem` →
/// `GetDeviceCaps(LOGPIXELSY)` → Unknown（调用方按 96 / scale 1.0 处理）。
/// 任一上级返回 0 或解析不到即**继续往下一级**，不留「取到 0 就当 1.0」的死路。
/// @param hwnd 窗口句柄；nullptr 表示建窗期（跳过按窗口那一级）。
/// @param desired 建窗期的期望落位矩形（工作区），供按显示器那一级定位；可为 nullptr。
/// @param api 已解析的 DPI API（生产路径传 `resolve_dpi_api()`，单测注入各级可用性）。
/// @return 读数与来源；`dpi == 0` 即各级均不可得。
[[nodiscard]] inline auto read_dpi(HWND hwnd, const RECT *desired, const DpiApi &api) -> DpiReading {
    // 1. 按窗口：句柄就绪后的首选（跨屏迁移后与所在屏一致）。
    if ((hwnd != nullptr) && (api.for_window != nullptr)) {
        const UINT dpi = api.for_window(hwnd);
        if (dpi > 0) {
            return DpiReading{.dpi = static_cast<int>(dpi), .source = DpiSource::Window};
        }
        // **取到 0 也算失败**：旧判据在此处直接返回 0，把「句柄尚未创建」与「读到了 96」混为一谈。
    }
    // 2. 按显示器：建窗期（`hwnd == nullptr`）的主路径。
    if (api.for_monitor != nullptr) {
        const HMONITOR mon = monitor_for_creation(desired);
        if (mon != nullptr) {
            UINT dpi_x = 0;
            UINT dpi_y = 0;
            // MDT_EFFECTIVE_DPI == 0：要的是「系统实际用于缩放的那个值」，不是原始角点值
            // （MinGW 的 shellscalingapi.h 未必给该枚举，故按官方文档数值写死）。
            if (api.for_monitor(mon, 0, &dpi_x, &dpi_y) == S_OK) {
                if (dpi_y > 0) {
                    return DpiReading{.dpi = static_cast<int>(dpi_y), .source = DpiSource::Monitor};
                }
                if (dpi_x > 0) {
                    return DpiReading{.dpi = static_cast<int>(dpi_x), .source = DpiSource::Monitor};
                }
            }
        }
    }
    // 3. 按系统：`GetDpiForSystem`（Win10 1607+）。旧代码里这一支结构上不可达，现在可达了。
    if (api.for_system != nullptr) {
        const UINT dpi = api.for_system();
        if (dpi > 0) {
            return DpiReading{.dpi = static_cast<int>(dpi), .source = DpiSource::System};
        }
    }
    // 4. 老系统兜底：屏幕 DC 的 `LOGPIXELSY`（Win32 两轴同值，故只取 Y 轴）。
    const HDC dc = GetDC(hwnd);
    if (dc != nullptr) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        if (dpi > 0) {
            return DpiReading{.dpi = dpi, .source = DpiSource::DeviceCaps};
        }
    }
    return DpiReading{};
}

/// @brief DPI → 缩放因子（唯一换算口径：96 DPI == 1.0）。
/// @param dpi DPI 读数；<= 0 按 96 处理（不猜）。
/// @return 缩放因子。
[[nodiscard]] inline constexpr auto dpi_to_scale(int dpi) -> float {
    return dpi > 0 ? static_cast<float>(dpi) / 96.0F : 1.0F;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
