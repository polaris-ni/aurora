#pragma once

#include <memory>
#include <string>

#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/window/surface.h"      // WindowStyleOptions / EventHandler / WindowStateHandler / WindowModeHandler
#include "aurora/window/window_state.h" // WindowState / WindowMode

// 共享 Win32 窗口宿主：仅依赖 Windows SDK（user32/gdi32），零三方依赖。
// 把「窗口创建 / 消息泵 / 事件翻译 / DPI / WM_PAINT·WM_SIZE 同步重渲染 / 白闪修复刷」
// 从 Win32Surface 抽取出来，供 Win32Surface(GDI) 与 D3D11Surface(GPU) 共用，
// 避免复制消息泵带来的行为分歧。
//
// pimpl 封装：公共头不再包含 <windows.h> / <windowsx.h>，所有 Win32/GDI 细节（HWND/HINSTANCE/
// 消息分发 / 键码映射 / UTF-8 转换等）移入 src/aurora/window/win32_window.cpp 的 Impl，
// 仅暴露 `std::unique_ptr<Impl> m_pimpl`；跨平台消费者（如 D3D11Surface、Headless 测试）
// 无需拉入重型平台头。原生句柄以 `void*` 暴露（避免公共头引入 <windows.h>），
// 调用方如需真实 `HWND` 显式 `static_cast` 即可。整文件被 #ifdef AURORA_BACKEND_WIN32 包裹。
#ifdef AURORA_BACKEND_WIN32

namespace aurora {

/// @brief 共享 Win32 窗口宿主：窗口创建 + 消息泵 + 事件翻译 + DPI + 同步重渲染。
///
/// 不含「像素如何上屏」：present 由 Win32Surface(GDI SetDIBitsToDevice) /
/// D3D11Surface(纹理上传) 各自实现，宿主仅在 WM_SIZE/WM_PAINT 时调用
/// `m_present_request` 触发 Window 的同步重渲染（消除最大化白闪）。
class Win32Window {
  public:
    using EventHandler = std::function<void(Event &)>;
    using WindowStateHandler = std::function<void(WindowState)>;
    using WindowModeHandler = std::function<void(WindowMode)>;
    using PresentRequest = std::function<void()>;

    /// @brief 创建原生窗口（带高级样式）。
    Win32Window(int w, int h, const std::string &title, const WindowStyleOptions &style);
    ~Win32Window();

    Win32Window(const Win32Window &) = delete;
    auto operator=(const Win32Window &) -> Win32Window & = delete;
    Win32Window(Win32Window &&) = delete;
    auto operator=(Win32Window &&) -> Win32Window & = delete;

    auto set_event_handler(EventHandler h) const -> void;
    auto set_window_state_handler(WindowStateHandler h) const -> void;
    auto set_window_mode_handler(WindowModeHandler h) const -> void;
    auto set_present_request(PresentRequest h) const -> void;

    /// @brief 运行时更新窗口标题（SetWindowText + UTF-8→ACP，支持非 ASCII 字符）。
    auto set_title(const std::string &title) const -> void;

    /// @brief 原生窗口句柄（`HWND`），以 `void*` 暴露以避免公共头引入 <windows.h>。
    [[nodiscard]] auto hwnd() const -> void *;
    [[nodiscard]] auto size() const -> Size;
    [[nodiscard]] auto scale_factor() const -> float;
    [[nodiscard]] auto should_close() const -> bool;
    /// @brief 已呈现次数（测试/自检用）：验证 WM_SIZE/WM_PAINT 触发了同步重渲染。
    [[nodiscard]] auto present_count() const -> int;
    /// @brief 类背景擦除刷（测试/自检用）：非空表示已消除最大化黑屏。
    [[nodiscard]] static auto background_brush() -> void *;

    /// @brief 轮询平台原生事件（PeekMessage 抽出消息泵，翻译后上抛）。
    auto poll_platform_events() const -> void;

    /// @brief 阻塞等待消息或超时：`MsgWaitForMultipleObjectsEx`
    /// 带 `QS_ALLINPUT|MWMO_INPUTAVAILABLE`，队列已有消息时立即返回。
    /// `timeout_ms < 0` 为无限等待，按 1000ms 分段兜底（防丢唤醒死等）。
    auto wait_events(double timeout_ms) const -> void;

    /// @brief 跨线程唤醒：`PostMessage(WM_NULL)` 使阻塞在 `wait_events` 的主循环立即返回。
    /// PostMessage 本身线程安全，可由后台线程（async 回投）直接调用。
    auto request_wake() const -> void;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_pimpl;
};

} // namespace aurora

#endif // AURORA_BACKEND_WIN32
