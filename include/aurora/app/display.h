#pragma once

#include <string>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/window/window.h"

namespace aurora {

/// @brief 一块物理显示器（逻辑像素与 DPI 信息，§ 平台 Shell / 多显示器）。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct Display {
    int id = 0;                ///< 平台分配的稳定标识（<0 表示无主/默认屏）
    std::string name;          ///< 显示器名称（如 "\\.\DISPLAY1"）
    Rect bounds;               ///< 整个显示器区域（物理像素，与 Windows 虚拟屏幕坐标系一致，含任务栏）
    Rect work_area;            ///< 工作区（物理像素，排除系统任务栏/停靠栏）
    float scale_factor = 1.0f; ///< DPI 缩放（dpi/96），用于 hidpi 适配
    bool is_primary = false;   ///< 是否主显示器
};

namespace app {

/// @brief 枚举当前所有显示器（含主屏）。无显示器时返回单默认屏。
[[nodiscard]] auto list_displays() -> std::vector<Display>;

/// @brief 返回主显示器；无显示器时返回单默认屏（1920x1080, scale 1, primary）。
[[nodiscard]] auto primary_display() -> Display;

/// @brief 将窗口迁移到指定 id 的显示器。
/// Win32 下以 SetWindowPos 将窗口居中到该显示器工作区；无头/非 Win32 为 no-op。
/// 找不到 id 时回退到主显示器。
auto move_window_to_display(Window &win, int display_id) -> void;

/// @brief 返回包含给定点的显示器；落点不在任何显示器内时回退主显示器。
[[nodiscard]] auto display_containing(Point p) -> Display;

} // namespace app
} // namespace aurora
