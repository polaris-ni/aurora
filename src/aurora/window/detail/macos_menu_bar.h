#pragma once

// macOS 原生菜单栏桥：**内部头**（与 `win32_menu_bar.h` / `macos_drag.h` 同列于 src/）。
//
// 机制：`NSMenu` / `NSMenuItem` 建树并挂到 `NSApp.mainMenu`；点击经 target-action 回到
// `AuroraMacMenuTarget`，按 `representedObject` 携带的模型 `id` 调注入的路由回调。菜单对象的
// 构建与点击处理全在本桥，折算层的次序与字段不做任何二次解释。
//
// 与 Win32 桥的一处平台差异（已申报）：`NSMenu` 是**应用级**对象，不挂在某个窗口上——同一份
// 菜单在所有窗口间共享，关掉任一窗口不会夺走菜单，最后一个窗口销毁后菜单也仍在（macOS 应用
// 在全窗口关闭后继续运行是常态）。故本桥没有宿主守卫与引用计数：`ensure_registered()` 在首个
// 窗口创建时把渲染通路登记到折算层一次，通路随进程存活。
//
// 快捷键文本：`MenuItem::shortcut_text` 的契约是「仅显示」。Win32 用 `\t` 让系统右对齐显示；
// AppKit 的右对齐位是 `keyEquivalent`，但那是**可触发**的快捷键而不是显示位。本桥不把显示
// 文本猜成 keyEquivalent（"Ctrl+O" 与 macOS 的 "Cmd+O" 语义不同，猜错会引入用户没声明的
// 快捷键），而是把 `shortcut_text` 原样接在标题后（`"\t"` 分隔），与折算层「字段保真」同口径。
//
// 勾选态：与 Win32 菜单项一致，点击可勾选项时先就地翻转 `NSMenuItem.state`，再回投 `id`；
// 应用侧以命令状态为准，重装菜单时以模型里的 `checked` 覆盖。
//
// 门控与 `macos_surface.cpp` 同款：macOS 平台宏 ∧ 后端宏；其它平台本文件编译为空 TU。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

namespace aurora::detail {

/// @brief 幂等自举：把 `NSMenu` 渲染通路登记到折算层（进程级一次，随进程存活）。
/// @return 恒为 true（登记是无失败路径的进程态操作）。
[[nodiscard]] auto macos_menu_bar_ensure_registered() -> bool;

/// @brief 当前是否已安装菜单栏（真机探针与诊断观测点，非公共 API）。
/// @return 最近一次 `install` 成功且尚未 `uninstall` 为 true。
[[nodiscard]] auto macos_menu_bar_is_installed() -> bool;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
