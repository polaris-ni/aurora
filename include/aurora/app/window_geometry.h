#pragma once

#include <optional>
#include <string>
#include <vector>

#include "aurora/app/display.h"
#include "aurora/core/json.h"
#include "aurora/core/types.h"
#include "aurora/preferences/preferences.h"
#include "aurora/window/window_state.h"

namespace aurora {

/// @brief 窗口几何快照：位置 / 尺寸 / 几何态 / 所在显示器（多窗口几何持久化的数据单元）。
///
/// 坐标系与 `app::Display` 一致——**屏幕物理像素**（非逻辑 dp）：持久化的目的正是「下次启动
/// 放回同一位置」，而 DPI 会随显示器变化，只有物理坐标是稳定参照。恢复时按最新 DPI 重新
/// 解释为逻辑尺寸。
///
/// 窗口组（多个窗口一起记忆）由调用方用 `Preferences::group("windows")` 分组建键实现，
/// 例如 `load_window_geometry(prefs.group("windows"), "main")`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none（save/load 才触碰 Preferences）
/// @note Rebuildable: yes, via `window_geometry_from_json`
struct WindowGeometry {
    Point origin{};  ///< 屏幕坐标（物理像素，与 `app::Display` 同一坐标系）
    Size size{};  ///< 外框尺寸（物理像素）
    WindowMode mode = WindowMode::Normal;  ///< 几何态（最大化/最小化/全屏需一并记忆）
    int display_id = -1;  ///< 所在显示器 id（与 `app::Display::id` 同源；-1 = 未知）
};

/// @brief 序列化为 JSON 对象（键稳定，供人工排查与跨版本兼容读取）。
/// @param g 待序列化的几何快照。
/// @return 含 `origin_x` / `origin_y` / `width` / `height` / `mode` / `display_id` 六键的对象。
[[nodiscard]] auto window_geometry_to_json(const WindowGeometry &g) -> json::Value;

/// @brief 从 JSON 反序列化：键缺失或类型不符返回 `std::nullopt`（异常不跨 API 边界）。
/// @param j 待解析的 JSON（须为含全部六键的对象；`mode` 越界 [0,3] 也视为格式错误）。
/// @return 解析出的几何快照；格式错误时为 `std::nullopt`。
[[nodiscard]] auto window_geometry_from_json(const json::Value &j) -> std::optional<WindowGeometry>;

/// @brief 几何是否可用：尺寸为正，且与给定显示器列表中的**任一工作区**有交集（允许部分越界）。
/// 判据刻意宽松：只要窗口有一部分可见就算可用（多屏拼接、任务栏遮挡等场景下不应拒绝恢复）。
/// 完全落在屏幕外（显示器被拔除、分辨率变小）或尺寸非正 → 不可用，调用方应回退默认几何。
/// @param g 待校验的几何快照。
/// @param displays 显示器列表（用其 `work_area` 做交集判定）。
/// @return 宽高均为正且与任一工作区有交集时为 `true`。
[[nodiscard]] auto is_window_geometry_usable(const WindowGeometry &g, const std::vector<Display> &displays) -> bool;

/// @brief 同上，显示器列表取当前系统的 `app::list_displays()`。
/// @param g 待校验的几何快照。
/// @return 与当前任一显示器工作区有交集且尺寸为正时为 `true`。
[[nodiscard]] auto is_window_geometry_usable(const WindowGeometry &g) -> bool;

/// @brief 保存窗口几何到 `Preferences`（键 `key`；仅写内存，落盘由调用方 `flush()` 决定）。
/// @param prefs 目标偏好存储。
/// @param key 存储键名。
/// @param g 待保存的几何快照（经 `window_geometry_to_json` 序列化后写入）。
auto save_window_geometry(preferences::Preferences &prefs, const std::string &key, const WindowGeometry &g) -> void;

/// @brief 读取窗口几何；键缺失、格式错误或不可用时返回 `std::nullopt`。
/// @param prefs 来源偏好存储。
/// @param key 存储键名。
/// @return 解析且可用性校验通过的几何快照；否则 `std::nullopt`（两类失败均记 WARN 日志以区分）。
[[nodiscard]] auto load_window_geometry(preferences::Preferences &prefs, const std::string &key)
    -> std::optional<WindowGeometry>;

/// @brief 同上，但作用于分组视图（窗口组：一组窗口共用一个分组，键区分各窗口）。
/// @code
///   auto wins = prefs.group("windows");
///   save_window_geometry(wins, "main", main_geo);
///   save_window_geometry(wins, "prefs", prefs_geo);
/// @endcode
/// @param group 分组视图（`Preferences::group("windows")` 的返回值）。
/// @param key 组内键名（区分各窗口）。
/// @param g 待保存的几何快照。
auto save_window_geometry(preferences::Preferences::Group group, const std::string &key, const WindowGeometry &g)
    -> void;

/// @brief 分组视图下的读取（语义同 `Preferences` 重载）。
/// @param group 分组视图（const 引用形态）。
/// @param key 组内键名。
/// @return 解析且可用性校验通过的几何快照；键缺失/格式错误/不可用时 `std::nullopt`。
[[nodiscard]] auto load_window_geometry(const preferences::Preferences::Group &group, const std::string &key)
    -> std::optional<WindowGeometry>;

}  // namespace aurora
