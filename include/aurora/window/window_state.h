#pragma once

#include <string>

namespace aurora {

/**
 * @brief 窗口可见性状态（窗口/应用级生命周期，与 `WindowMode` 正交）。
 *
 * 覆盖「窗口最小化 / 被其他应用遮挡 / 前台激活」等场景。所有主流桌面框架
 * （Qt/WPF/Win32/Flutter/Web）都只在窗口或应用层报告可见性，从不 per-widget；
 * 精确「被其他窗口像素级遮挡」检测昂贵且普遍不被框架支持，故以「失焦 = 被遮挡」近似为 `Occluded`。
 *
 * 跨框架对照：
 * - `Visible` ≈ Android `Activity.onResume` ≈ Flutter `AppLifecycleState.resumed` ≈ 前台激活窗口；
 * - `Occluded` ≈ Android `Activity.onPause`（仍可见但非前台）≈ Web `window` blur /
 * `visibilityState=visible&hidden=false`；
 * - `Hidden` ≈ Android `Activity.onStop`（不可见）≈ Flutter `AppLifecycleState.paused` ≈ 窗口最小化。
 */
enum class WindowState : std::uint8_t {
    Visible,  ///< 前台激活且可见
    Occluded, ///< 可见但失焦（被其他窗口遮挡 / 非激活窗口）
    Hidden,   ///< 最小化或不可见
};

/// @brief 人类可读名（调试/序列化用）。
[[nodiscard]] inline auto to_string(WindowState s) -> std::string {
    switch (s) {
    case WindowState::Visible: return "Visible";
    case WindowState::Occluded: return "Occluded";
    case WindowState::Hidden: return "Hidden";
    }
    return "Visible";
}

/**
 * @brief 窗口几何态（与 `WindowState` 正交的独立维度）。
 *
 * 最大化/最小化/全屏是「窗口几何/状态」关切，与可见性正交：例如最大化时 `WindowState` 仍为
 * `Visible`；最小化时同时满足 `WindowState::Hidden`。Qt 的 `WindowMaximized` 位标志、Win32 的
 * `SIZE_MAXIMIZED`、WPF 的 `WindowState.Maximized` 均将其作为独立窗口状态。
 */
enum class WindowMode : std::uint8_t {
    Normal,     ///< 普通窗口
    Maximized,  ///< 最大化
    Minimized,  ///< 最小化（同时使 WindowState=Hidden）
    FullScreen, ///< 全屏
};

/// @brief 人类可读名（调试/序列化用）。
[[nodiscard]] inline auto to_string(WindowMode m) -> std::string {
    switch (m) {
    case WindowMode::Normal: return "Normal";
    case WindowMode::Maximized: return "Maximized";
    case WindowMode::Minimized: return "Minimized";
    case WindowMode::FullScreen: return "FullScreen";
    }
    return "Normal";
}

/// @brief 由「是否最小化」与「是否前台激活」推导窗口可见性状态（统一 Win32/GLFW 映射，可单测）。
/// @param minimized 窗口是否最小化（WM_SIZE SIZE_MINIMIZED / GLFW iconify）。
/// @param active    窗口是否前台激活（WM_ACTIVATE 非 WA_INACTIVE / GLFW focus）。
[[nodiscard]] inline auto compute_window_state(bool minimized, bool active) -> WindowState {
    if (minimized) {
        return WindowState::Hidden;
    }
    return active ? WindowState::Visible : WindowState::Occluded;
}

/// @brief 由最小化/最大化/全屏标志推导窗口几何态（统一 Win32/GLFW 映射，可单测）。
/// @note 优先级：全屏 > 最小化 > 最大化 > 普通。Win32 无原生全屏区分，由后端显式传入 `fullscreen`。
[[nodiscard]] inline auto compute_window_mode(bool minimized, bool maximized, bool fullscreen) -> WindowMode {
    if (fullscreen) {
        return WindowMode::FullScreen;
    }
    if (minimized) {
        return WindowMode::Minimized;
    }
    if (maximized) {
        return WindowMode::Maximized;
    }
    return WindowMode::Normal;
}

} // namespace aurora
