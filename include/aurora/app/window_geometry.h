#pragma once

#include <optional>
#include <string>
#include <vector>

#include "aurora/app/display.h"
#include "aurora/core/types.h"
#include "aurora/preferences/preferences.h"
#include "aurora/widget/props_io.h"
#include "aurora/window/window_state.h"

namespace aurora {

/**
 * @brief 窗口几何快照：位置 / 尺寸 / 几何态 / 所在显示器（多窗口几何持久化的数据单元）。
 *
 * 坐标系与 `app::Display` 一致——**屏幕物理像素**（非逻辑 dp）：持久化的目的正是「下次启动
 * 放回同一位置」，而 DPI 会随显示器变化，只有物理坐标是稳定参照。恢复时按最新 DPI 重新
 * 解释为逻辑尺寸。
 *
 * 窗口组（多个窗口一起记忆）由调用方用 `Preferences::group("windows")` 分组建键实现，
 * 例如 `load_window_geometry(prefs.group("windows"), "main")`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none（save/load 才触碰 Preferences）
 * @note Rebuildable: yes, via `window_geometry_from_json`
 */
struct WindowGeometry {
    Point origin{};                        ///< 屏幕坐标（物理像素，与 `app::Display` 同一坐标系）
    Size size{};                           ///< 外框尺寸（物理像素）
    WindowMode mode = WindowMode::Normal;  ///< 几何态（最大化/最小化/全屏需一并记忆）
    int display_id = -1;                   ///< 所在显示器 id（与 `app::Display::id` 同源；-1 = 未知）
};

/// @brief 序列化为 JSON 对象（键稳定，供人工排查与跨版本兼容读取）。
[[nodiscard]] auto window_geometry_to_json(const WindowGeometry &g) -> Json;

/// @brief 从 JSON 反序列化：键缺失或类型不符返回 `std::nullopt`（异常不跨 API 边界）。
[[nodiscard]] auto window_geometry_from_json(const Json &j) -> std::optional<WindowGeometry>;

/// @brief 几何是否可用：尺寸为正，且与给定显示器列表中的**任一工作区**有交集（允许部分越界）。
///
/// 判据刻意宽松：只要窗口有一部分可见就算可用（多屏拼接、任务栏遮挡等场景下不应拒绝恢复）。
/// 完全落在屏幕外（显示器被拔除、分辨率变小）或尺寸非正 → 不可用，调用方应回退默认几何。
[[nodiscard]] auto is_window_geometry_usable(const WindowGeometry &g, const std::vector<Display> &displays) -> bool;

/// @brief 同上，显示器列表取当前系统的 `app::list_displays()`。
[[nodiscard]] auto is_window_geometry_usable(const WindowGeometry &g) -> bool;

/// @brief 保存窗口几何到 `Preferences`（键 `key`；仅写内存，落盘由调用方 `flush()` 决定）。
auto save_window_geometry(preferences::Preferences &prefs, const std::string &key, const WindowGeometry &g) -> void;

/// @brief 读取窗口几何；键缺失、格式错误或不可用时返回 `std::nullopt`。
[[nodiscard]] auto load_window_geometry(preferences::Preferences &prefs, const std::string &key)
    -> std::optional<WindowGeometry>;

/// @brief 同上，但作用于分组视图（窗口组：一组窗口共用一个分组，键区分各窗口）。
/// @code
///   auto wins = prefs.group("windows");
///   save_window_geometry(wins, "main", main_geo);
///   save_window_geometry(wins, "prefs", prefs_geo);
/// @endcode
auto save_window_geometry(preferences::Preferences::Group group, const std::string &key, const WindowGeometry &g)
    -> void;

/// @brief 分组视图下的读取（语义同 `Preferences` 重载）。
[[nodiscard]] auto load_window_geometry(preferences::Preferences::Group group, const std::string &key)
    -> std::optional<WindowGeometry>;

}  // namespace aurora
