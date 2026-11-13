#pragma once

// macOS 桌面外壳集成桥：**内部头**（src/aurora/app/detail/）。本文件是 ObjC++ TU 的头。
//
// 三项注册共享同一硬前提：**只在 `.app` bundle 内生效**。
// - URL 方案：`CFBundleURLTypes` 声明 + `LSSetDefaultHandlerForURLScheme` 把自有方案指回本应用；
//   不在 bundle 内 → `url-scheme-failed`，detail 点名缺 bundle；
// - 文件关联：`CFBundleDocumentTypes`（UTI）声明 + Launch Services 查询默认处理程序；
// - 登录项：`SMAppService`（macOS 13+，要求 bundle）；沙箱形态如实报 `Unsupported`；
// - 打印会话：AppKit `NSPrintOperation`（隐藏打印面板与进度面板，页序由自管视图驱动）；
// - 单实例：用户私有临时目录下的 Unix 套接字（直接二次启动的进程移交），并在 AppKit 已就绪时
//   登记 `kAEGetURL` Apple Event 处理器（经 URL 方案被唤起时的系统通道）。
//
// 门控与既有 macOS 桥一致（平台宏 ∧ 后端宏）；非 Apple / 未开 macOS 后端时编译为空 TU。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

namespace aurora::detail {

/// @brief 幂等登记 macOS 外壳集成平台桥。
/// @return 恒为 true。
[[nodiscard]] auto macos_shell_integration_ensure() -> bool;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
