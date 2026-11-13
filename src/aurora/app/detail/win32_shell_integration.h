#pragma once

// Win32 桌面外壳集成桥：**内部头**（src/aurora/app/detail/）。
//
// 一个 TU 收敛五项能力在 Windows 上的落法，彼此共用同一份注册表访问与用户名解析：
// - URL 方案：`HKCU\Software\Classes\<scheme>`（用户级，不碰 `HKLM`）；
// - 文件关联：`HKCU\Software\Classes\Aurora.<ext>` ProgID + `RegisteredApplications` +
//   Capabilities（只写 ProgID 会在「打开方式」里查不到本应用）；
// - 登录项：`HKCU\...\CurrentVersion\Run`（用户级；`StartupApproved` 的禁用记录同向覆盖）；
// - 单实例：`Local\` 有名互斥量 + 消息窗口 `WM_COPYDATA` 转交；
// - 打印会话：GDI `StartDoc` / `StartPage` / `EndDoc`。
//
// 与 `win32_drag.h` / `win32_ime.h` 同列：公共头不引 `<windows.h>`，声明在下、实现在独立 TU；
// 门控是平台宏（Windows 上任何后端都可用这些 OS 级能力，与 Surface 种类无关）。
#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS

namespace aurora::detail {

/// @brief 幂等登记全部 Win32 外壳集成平台桥（进程级单槽，重复调用无副作用）。
/// @return 恒为 true（登记是无失败路径的进程态操作；单次能力失败在调用时走 `Result`）。
[[nodiscard]] auto win32_shell_integration_ensure() -> bool;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS
