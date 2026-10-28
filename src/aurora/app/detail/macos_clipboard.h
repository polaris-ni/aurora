#pragma once

// macOS 剪贴板富格式后端：**内部头**（与 `clipboard_backend.h` 同列于 src/）。
//
// 机制：`NSPasteboard` 的 general pasteboard——macOS 上剪贴板内容的唯一载体。本桥把门面的
// 函数表（`clipboard::detail::PlatformBackend`）逐项落到 `NSPasteboard`：
// - 原始载荷：MIME → pasteboard type（`text/plain` → `public.utf8-plain-text`，`image/png` →
//   `public.png`，`text/html` → `public.html`，其余以 MIME 字面量作自定义 type）；
// - 文件清单：`NSURL` 数组（`public.file-url`）+ 同一次写入附带 `text/uri-list` 文本；
// - 惰性提供：`NSPasteboardItem` + `NSPasteboardItemDataProvider`，格式声明与 `provide` 回调一一对应；
// - 所有权丢失：`pasteboardFinishedWithDataProvider:` 在「全部数据已提供」**或「剪贴板所有权易主」**
//   时送达，用写入后记录的 `changeCount` 区分两者——只有后者才触发 `fire_ownership_lost`。
//
// 两处如实申报的平台差异：
// 1) **无变更监听**：`NSPasteboard` 只有 `changeCount` 轮询，没有系统变更通知，故本后端不填
//    `set_change_listener`，`Clipboard::on_change` 在 macOS 上照旧返回 `clipboard-listen-unsupported`
//    （轮询不是通知机制，填上便是谎报）。
// 2) **整块替换**：每次写入都先 `clearContents`（与 Win32 平台路径「后写者接管整块剪贴板」同口径），
//    `clear()` 仍走 `pbcopy` 空输入重写（门面既有实现）。
//
// 门控与 `macos_recent_documents.h` 同款：macOS 平台宏 ∧ 后端宏；其它平台本文件编译为空 TU。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

namespace aurora::detail {

/// @brief 幂等自举：把 `NSPasteboard` 富格式后端登记到门面的进程级单槽。
/// @return 恒为 true（登记是无失败路径的进程态操作）。
[[nodiscard]] auto macos_clipboard_ensure_registered() -> bool;

/// @brief 当前是否持有惰性提供要约（真机探针与诊断观测点，非公共 API）。
/// @return 存在未撤销的惰性要约时为 true。
[[nodiscard]] auto macos_clipboard_has_offer() -> bool;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS
