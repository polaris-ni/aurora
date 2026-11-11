#pragma once

// macos_click_config.h —— macOS 连击时间窗的「读取 + 折算」真源（库实现可见，不属公共 API）。
//
// 为何单列：公共契约 `os_double_click_ms()`（`include/aurora/event/event.h`）自诞生起就把 macOS 划进
// 「无可零依赖读取的公开 API，回退库级常量」那一档，实测该断言不成立——AppKit 自己就发布
// `+[NSEvent doubleClickInterval]`，返回用户设定的秒值且无需 `NSApplication` 就绪（本机实测裸可执行
// 进程读到 0.5s）。于是派发器的连击窗口在 macOS 上永远拿的是库内常量，用户把双击调快调慢都无感。
// 本层把这条腿补上，并把「秒 → 毫秒」的折算抽成零平台头的纯函数：ObjC++ 侧只剩一次读取，
// 折算表因此可以在任何 runner 上表驱动钉住（与 `macos_decoration.h` 的拆层口径同族）。
//
// 折算口径与 Windows / Linux 两支一致，不另立一套阈值：不可读即回退调用方给的兜底常量，可读即落
// 毫秒并保证返回值 ≥ 1（公共契约写「恒为正」）。
//
// 本层边界（刻意）：
// - 纯函数不 include 任何平台头，非 Apple 构建同样能编译与单测；
// - 不做缓存：每次调用都现读，运行期改系统设置后重建派发器即反映新值（与 Windows 支同语义）；
// - 不引入新公共类型：公共面仍是那枚既有的 `os_double_click_ms()`，本批只改它的 macOS 分支。

#include <cmath>
#include <cstdint>

namespace aurora::detail {

/// @brief 把 OS 报出的双击间隔（秒，浮点）折算成派发器使用的连击时间窗（毫秒）。
/// @param seconds AppKit `+[NSEvent doubleClickInterval]` 一类的秒值；非正或非有限视为不可读。
/// @param fallback_ms 不可读时回退的库级兜底常量（调用方传 `AURORA_DEFAULT_CLICK_WINDOW_MS`）。
/// @return 连击时间窗（毫秒）：不可读取 `fallback_ms`，可读按四舍五入落毫秒，亚毫秒夹为 1。
[[nodiscard]] inline auto click_window_ms_from_seconds(double seconds, std::uint32_t fallback_ms) -> std::uint32_t {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return fallback_ms;  // 不可读 / 非正 / NaN / Inf：一律走调用方的库级兜底，不猜系统值
    }
    const double ms = seconds * 1000.0;
    if (ms < 1.0) {
        return 1U;  // 系统把间隔压到亚毫秒：仍留「可判定连击」的最小正窗，返回 0 会关掉整条连击链
    }
    return static_cast<std::uint32_t>(std::llround(ms));
}

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

/// @brief 读 macOS 系统双击速度并折算成连击时间窗（`+[NSEvent doubleClickInterval]` 的封装腿）。
///
/// 实现落在 `macos_click_config.cpp`（ObjC++ TU，语言由 `cmake/AuroraBackends.cmake` 的 macOS 清单标记）；
/// 声明留在这里使 `click_config.cpp` 这份纯 C++ 实现文件不必触碰 AppKit 头。
/// @return 连击时间窗（毫秒，≥ 1）；AppKit 报出非正 / 非有限值时回退 `AURORA_DEFAULT_CLICK_WINDOW_MS`。
[[nodiscard]] auto macos_double_click_ms() -> std::uint32_t;

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS

}  // namespace aurora::detail
