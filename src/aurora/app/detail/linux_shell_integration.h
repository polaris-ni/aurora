#pragma once

// Linux 桌面外壳集成桥：**内部头**（src/aurora/app/detail/）。
//
// 三处注册都落在 freedesktop 用户目录里，单实例走用户私有运行时目录：
// - URL 方案：`$XDG_DATA_HOME/applications/<id>.desktop` 携带
//   `MimeType=x-scheme-handler/<scheme>`，并在 `$XDG_CONFIG_HOME/mimeapps.list` 的
//   `[Default Applications]` 里登记归属（自有方案，登记给自己是语义本身）；
// - 文件关联：桌面条目 `MimeType=` 与 `mimeapps.list` 的 `[Added Associations]`（只声明候选，
//   不写 `[Default Applications]`——不抢默认处理程序）；
// - 登录项：`$XDG_CONFIG_HOME/autostart/<id>.desktop`，启用 / 禁用表达在同一文件的 `Hidden=`；
// - 单实例：`$XDG_RUNTIME_DIR/aurora-<key>/`（`0700`）下的锁文件 + Unix 套接字，崩溃后按
//   文件锁自然回收；**不用** `/tmp` 下可预测的名字。
//
// 打印会话本平台不接线（不引入 libcups，见 `06-app-platform.md` 的未决项结论）。
#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_LINUX

namespace aurora::detail {

/// @brief 幂等登记 Linux 外壳集成平台桥（打印槽位不登记，故 `print_targets()` 如实报
///        `print-session-unsupported`）。
/// @return 恒为 true。
[[nodiscard]] auto linux_shell_integration_ensure() -> bool;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX
