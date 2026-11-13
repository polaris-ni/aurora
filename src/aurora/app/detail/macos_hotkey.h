#pragma once

/// @brief macOS 全局热键的 Carbon 接线常量（实现与验收探针共用，避免签名漂移）。
///
/// Carbon 的 `EventHotKeyID` 用「四字符签名 + 进程内 ID」标识热键；签名取 `'AURA'`
/// （十六进制字面量，避免多字符字面量的实现定义值）。探针合成 `kEventHotKeyPressed`
/// 事件时须带同一签名，否则事件处理器会按「别的进程/别的子系统」丢弃。
/// @file macos_hotkey.h

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_MACOS

#include <cstdint>

namespace aurora::detail {

/// @brief Carbon 热键签名 `'AURA'`（0x41555241）。
inline constexpr std::uint32_t AURORA_MACOS_HOTKEY_SIGNATURE = 0x41555241U;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS
