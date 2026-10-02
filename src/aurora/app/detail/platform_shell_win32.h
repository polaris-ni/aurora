#pragma once

// 平台 Shell 的「隐藏消息窗口」内部单元（src/aurora/app/detail/）。
//
// 为什么单列：Windows 上有一批 OS 级能力（全局热键 `WM_HOTKEY`、会话/任务栏广播消息等）
// 都需要**一个属于本库的隐藏窗口**来接消息，但公共头不能引 `<windows.h>`，各能力单元
// 也不该各自建一套窗口（一个进程内多个消息窗互相抢不到消息，且类注册要各写一遍）。
// 故把「取（必要时建）进程共享的隐藏消息窗口」+「往该窗口挂消息钩子」两件事收敛在这里，
// 上层单元只认平台无关的类型与回调。
//
// 平台口径与 `system_tray_win32.cpp` 一致：本文件在全部目标上编译，Win32 专有代码用
// `AURORA_PLATFORM_WINDOWS`（不是原生 `_WIN32`，见 `core/platform.h`）在函数体内隔离，
// 非 Windows 目标上 `ensure_message_window()` 恒返回 nullptr、`add_message_hook()` 恒返回 0，
// 调用方据此走降级路径，不需要额外的 feature 宏。
//
// 消息钩子是**同步**回调：在消息泵的 `DispatchMessage` 内被调用。回调里只做「记录 + 排队」，
// 不要直接执行用户逻辑（重入会打乱帧循环的重排/重建假设）。

#include <cstdint>
#include <functional>

namespace aurora::internal {

/// @brief 隐藏消息窗口句柄（平台无关侧的类型擦除表示）。
/// Windows 下即 `HWND`；平台不支持或创建失败时恒为 nullptr。
using ShellWindow = void *;

/// @brief 一条原生消息的载荷（平台无关侧表示）。
struct ShellMessage {
    std::uintptr_t wparam = 0;  ///< 消息参数 WPARAM（Windows 下为 UINT_PTR）
    std::intptr_t lparam = 0;  ///< 消息参数 LPARAM（Windows 下为 LONG_PTR）
};

/// @brief 消息钩子：随隐藏窗口的消息泵同步回调。
/// @param message 消息编号（Windows 下为 `UINT`，如 `WM_HOTKEY`）。
/// @param payload 该消息的 WPARAM / LPARAM。
/// @return 消息已被消费（不再传给后续钩子、也不交给默认窗口过程）时为 `true`。
using ShellMessageHook = std::function<bool(std::uint32_t message, const ShellMessage &payload)>;

/// @brief 取进程共享的隐藏消息窗口；首次调用时创建（此后复用同一句柄）。
/// @return 窗口句柄；平台不支持或创建失败时为 nullptr。
/// @note Thread: main-thread only（窗口与其消息泵同线程）
[[nodiscard]] auto ensure_message_window() -> ShellWindow;

/// @brief 隐藏消息窗口是否可用（平台支持且能创建成功）。
/// @return 可用（已创建或可创建）时为 `true`；平台不支持时为 `false`。
/// @note Thread: main-thread only
[[nodiscard]] auto message_window_available() -> bool;

/// @brief 往隐藏消息窗口挂一个消息钩子（必要时先创建窗口）。
/// @param hook 钩子回调（返回 `true` 表示消费该消息）；空钩子直接返回 0。
/// @return 钩子 ID（自 1 起，供 `remove_message_hook` 注销）；平台不支持或建窗失败时为 0。
/// @note Thread: main-thread only
auto add_message_hook(ShellMessageHook hook) -> std::uint32_t;

/// @brief 注销一个消息钩子（ID 不存在或为 0 时静默返回 false）。
/// @param id `add_message_hook` 返回的钩子 ID。
/// @return 确有钩子被注销时为 `true`。
/// @note Thread: main-thread only
auto remove_message_hook(std::uint32_t id) -> bool;

}  // namespace aurora::internal
