#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "aurora/core/a11y_provider.h"
#include "aurora/core/types.h"
#include "aurora/event/event.h"
#include "aurora/window/surface.h"
#include "aurora/window/window_state.h"

// 共享 Win32 窗口宿主：仅依赖 Windows SDK（user32/gdi32），零三方依赖。
// 把「窗口创建 / 消息泵 / 事件翻译 / DPI / WM_PAINT·WM_SIZE 同步重渲染 / 白闪修复刷」
// 从 Win32Surface 抽取出来，供 Win32Surface(GDI) 与 D3D11Surface(GPU) 共用，
// 避免复制消息泵带来的行为分歧。
//
// pimpl 封装：公共头不再包含 <windows.h> / <windowsx.h>，所有 Win32/GDI 细节（HWND/HINSTANCE/
// 消息分发 / 键码映射 / UTF-8 转换等）移入 src/aurora/window/win32_host.cpp 的 Impl，
// 仅暴露 `std::unique_ptr<Impl> pimpl_`；跨平台消费者（如 D3D11Surface、Headless 测试）
// 无需拉入重型平台头。原生句柄以 `void*` 暴露（避免公共头引入 <windows.h>），
// 调用方如需真实 `HWND` 显式 `static_cast` 即可。整文件被 #ifdef AURORA_BACKEND_WIN32 包裹。
#ifdef AURORA_BACKEND_WIN32

namespace aurora {

/// @brief 共享 Win32 窗口宿主：窗口创建 + 消息泵 + 事件翻译 + DPI + 同步重渲染。
///
/// 不含「像素如何上屏」：present 由 Win32Surface(GDI SetDIBitsToDevice) /
/// D3D11Surface(纹理上传) 各自实现，宿主仅在 WM_SIZE/WM_PAINT 时调用
/// `present_request_` 触发 Window 的同步重渲染（消除最大化白闪）。
class Win32Host {
  public:
    using EventHandler = std::function<void(Event &)>;
    using WindowStateHandler = std::function<void(WindowState)>;
    using WindowModeHandler = std::function<void(WindowMode)>;
    using PresentRequest = std::function<void()>;

    /// @brief 创建原生窗口（带高级样式）。
    Win32Host(int w, int h, const std::string &title, const WindowStyleOptions &style);
    ~Win32Host();

    Win32Host(const Win32Host &) = delete;
    auto operator=(const Win32Host &) -> Win32Host & = delete;
    Win32Host(Win32Host &&) = delete;
    auto operator=(Win32Host &&) -> Win32Host & = delete;

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

    // ---- 父子窗口与模态（多窗口）----

    /// @brief 设置 owner 窗口（HWND，以 `void*` 传入；nullptr = 解除从属）。
    /// 走 `GWLP_HWNDPARENT`：子窗恒浮于 owner 之上、随 owner 最小化，且不产生独立任务栏条目。
    auto set_owner(void *owner_hwnd) const -> void;
    /// @brief 启用/禁用窗口输入（`EnableWindow`）——模态窗口屏蔽其 owner 时使用。
    auto set_enabled(bool on) const -> void;

    // ---- z 序、显示器与 DPI（多窗口）----

    /// @brief 提升 z 序到同组顶部（不激活）：`BringWindowToTop`。
    auto raise() const -> void;
    /// @brief 激活窗口（置顶 + 键盘焦点）：`SetForegroundWindow` + `SetFocus`。
    auto focus_window() const -> void;
    /// @brief 当前所在显示器的稳定 id（`MonitorFromWindow` 的 HMONITOR 句柄值，
    /// 与 `app::Display::id` 同源）；未知返回 -1。
    [[nodiscard]] auto display_id() const -> int;
    /// @brief 窗口在屏幕上的位置（**物理像素**，`GetWindowRect` 左上角）。
    [[nodiscard]] auto position() const -> Point;
    /// @brief 程序化移动窗口（`SetWindowPos`，物理像素）。
    auto set_position(Point p) const -> void;
    /// @brief 程序化设置窗口**外框**尺寸（`SetWindowPos`，物理像素）。
    auto set_size(Size s) const -> void;
    /// @brief 注册 DPI 缩放变化回调：`WM_DPICHANGED` 处理后上报新的 `scale_factor`。
    auto set_scale_change_handler(std::function<void(float)> h) const -> void;

    // ---- 无障碍桥（D13/D14/G14）----

    /// @brief 本窗口的无障碍桥（`a11y::Provider`）；未激活（尚无读屏查询）返回 nullptr。
    ///
    /// 桥实例**由窗口宿主持有**（而非任一 Surface）：`Win32Surface`(GDI) 与 `D3D11Surface`(GPU)
    /// 共用本宿主，二者 `Surface::accessibility_provider()` 都返回同一实例，避免两份
    /// id→Widget* 映射分裂。惰性：首个 `WM_GETOBJECT(UiaRootObjectId)` 到达时才构造并激活。
    [[nodiscard]] auto accessibility_provider() const -> a11y::Provider *;

    /// @brief 注入语义树根（每帧调用；桥尚未构造时由宿主记下，构造后补喂）。
    auto set_accessibility_root(Widget *root) const -> void;

    /// @brief 覆盖 `WM_GETOBJECT` 处理（宿主可抢先接管；默认空 → 走内置 UIA 桥）。
    ///
    /// 以指针宽度整数传递 `WPARAM`/`LPARAM`、返回 `LRESULT`，避免在公共头引入 `<windows.h>`；
    /// 宿主侧（`.cpp`）在调用前 `static_cast` 回原生类型。
    /// @param hook 返回 `std::nullopt` 表示「未处理，交内置桥 / DefWindowProc」。
    auto set_accessibility_hook(std::function<std::optional<std::intptr_t>(std::uintptr_t, std::intptr_t)> h) const
        -> void;

    // ---- 输入法桥（IMM32 组合输入）----

    /// @brief 注入 IME 候选窗定位查询（返回**窗口逻辑 dp** 零宽竖盒；默认空 → 系统默认位置）。
    ///
    /// 与 `Surface::set_composition_caret_provider` 同一契约：`Win32Surface`(GDI) 与
    /// `D3D11Surface`(GPU) 共用本宿主，宿主在 `WM_IME_STARTCOMPOSITION`/`WM_IME_COMPOSITION`
    /// 时调用它取当前焦点控件的插入点，再 `× scale` + `ClientToScreen` 喂 `ImmSetCandidateWindow`。
    auto set_composition_caret_provider(std::function<Rect()> provider) const -> void;

    /// @brief 是否处于 IME 组合中（真机验收探针 / 自检观测器用）。
    [[nodiscard]] auto ime_composing() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

}  // namespace aurora

#endif  // AURORA_BACKEND_WIN32
