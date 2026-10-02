// platform_shell_win32.cpp — 进程共享的隐藏消息窗口（HWND_MESSAGE）+ 消息钩子系统。
// - AURORA_PLATFORM_WINDOWS：真实 `RegisterClassExW` + `CreateWindowExW(HWND_MESSAGE)`。
// - 其它平台：`ensure_message_window()` 恒 nullptr、`add_message_hook()` 恒 0，调用方据此降级。
// 四个公共函数在所有目标上都定义（避免非 Win32 链接失败）；Win32 专有代码用内部 #ifdef 隔离。
//
// 注意：_WIN32_WINNT / _WIN32_IE 必须在任何 aurora 头文件之前定义（见 file_dialog_win32.cpp 注释）。
#include "aurora/core/platform.h"
#ifdef AURORA_PLATFORM_WINDOWS
#ifndef _WIN32_WINNT
// Windows SDK 版本旋钮，不可改名（见 platform.h 例外说明）
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
// Windows SDK 版本旋钮，不可改名
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_IE 0x0600
#endif
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif
#include <cstdint>
#include <utility>
#include <vector>

#include "aurora/app/detail/platform_shell_win32.h"
#include "aurora/core/log.h"

namespace aurora::internal {

namespace {

/// @brief 隐藏消息窗口的进程级状态：窗口句柄 + 钩子表。
struct ShellWindowState {
#ifdef AURORA_PLATFORM_WINDOWS
    HWND hwnd = nullptr;  ///< 隐藏消息窗口（HWND_MESSAGE，不显示、不进任务栏）
    std::vector<std::pair<std::uint32_t, ShellMessageHook>> hooks;  ///< 钩子表（按注册序）
    std::uint32_t next_hook_id = 1;  ///< 下一个可分配的钩子 ID（自 1 起，回收不复用）
#endif
    bool create_failed = false;  ///< 建窗已失败过：不再反复重试（无桌面会话时每次都会失败）
};

/// @brief 进程唯一状态实例（函数内 static，规避静态初始化顺序问题）。
/// 非 Windows 目标上本文件不引用它（各函数直接返回降级值），故标 maybe_unused 免告警。
[[maybe_unused]] [[nodiscard]] auto shell_state() -> ShellWindowState & {
    static ShellWindowState state;
    return state;
}

#ifdef AURORA_PLATFORM_WINDOWS
// 【豁免说明】Win32 窗口接线：句柄须以 reinterpret_cast 在 HWND 与 LONG_PTR 之间往返，
// 逐点抑制不成比例，故按区间豁免（名单取自去掉本区间后 clang-tidy 实测报出的检查集）。
// NOLINTBEGIN(*-pro-type-*, performance-no-int-to-ptr)

// 隐藏消息窗口的窗口过程：先过钩子表，无人消费再走默认过程。
// NOLINTNEXTLINE(modernize-use-trailing-return-type): CALLBACK 调用约定下尾返回类型会改变签名语义
LRESULT CALLBACK shell_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto &st = shell_state();
    // 钩子回调里可能注销自身（或别的钩子）：先把 ID 快照出来再逐个查找，
    // 直接按索引遍历会在表被改动后读到错位元素。
    std::vector<std::uint32_t> ids;
    ids.reserve(st.hooks.size());
    for (const auto &kv : st.hooks) {
        ids.push_back(kv.first);
    }
    for (const std::uint32_t id : ids) {
        for (const auto &kv : st.hooks) {
            if (kv.first != id) {
                continue;
            }
            const ShellMessage payload{static_cast<std::uintptr_t>(wp), static_cast<std::intptr_t>(lp)};
            if (kv.second && kv.second(static_cast<std::uint32_t>(msg), payload)) {
                return 0;
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// @brief 创建隐藏消息窗口（幂等：已创建则直接复用）。
/// @return 创建/复用成功为真；平台不支持或创建失败为假。
auto create_message_window() -> bool {
    auto &st = shell_state();
    if (st.hwnd != nullptr) {
        return true;
    }
    if (st.create_failed) {
        return false;
    }
    static constexpr const wchar_t *AURORA_SHELL_WINDOW_CLASS = L"AuroraShellMessageWindow";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &shell_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = AURORA_SHELL_WINDOW_CLASS;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            AURORA_LOG_WARN("shell", "RegisterClassExW(AuroraShellMessageWindow) failed");
        }
        registered = true;
    }
    st.hwnd = CreateWindowExW(0, AURORA_SHELL_WINDOW_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (st.hwnd == nullptr) {
        st.create_failed = true;
        AURORA_LOG_WARN("shell", "CreateWindowExW(HWND_MESSAGE) failed");
        return false;
    }
    return true;
}
// NOLINTEND(*-pro-type-*, performance-no-int-to-ptr)
#endif

}  // namespace

auto ensure_message_window() -> ShellWindow {
#ifdef AURORA_PLATFORM_WINDOWS
    if (!create_message_window()) {
        return nullptr;
    }
    return shell_state().hwnd;
#else
    return nullptr;
#endif
}

auto message_window_available() -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    return create_message_window();
#else
    return false;
#endif
}

auto add_message_hook(ShellMessageHook hook) -> std::uint32_t {
    if (!hook) {
        return 0;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    if (!create_message_window()) {
        return 0;
    }
    auto &st = shell_state();
    const std::uint32_t id = st.next_hook_id++;
    st.hooks.emplace_back(id, std::move(hook));
    return id;
#else
    return 0;
#endif
}

auto remove_message_hook(std::uint32_t id) -> bool {
    if (id == 0U) {
        return false;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    auto &st = shell_state();
    for (auto it = st.hooks.begin(); it != st.hooks.end(); ++it) {
        if (it->first == id) {
            st.hooks.erase(it);
            return true;
        }
    }
#endif
    return false;
}

}  // namespace aurora::internal
