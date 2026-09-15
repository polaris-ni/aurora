#include "aurora/app/window_geometry.h"

#include "aurora/core/log.h"

namespace aurora {

namespace {

/// @brief 读取数值字段；键缺失或类型不符时置 `ok=false` 并回退默认值。
auto number_or(const Json &j, const char *key, double fallback, bool *ok) -> double {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        *ok = false;
        return fallback;
    }
    return it->get<double>();
}

/// @brief 解析存储的几何并做可用性校验；任一环节不通过即返回 `nullopt`，并按原因给出 WARN。
///
/// 两类失败刻意区分：**格式错误**（键缺失/类型不符/枚举越界，通常意味着存储被外部破坏）
/// 与**几何不可用**（显示器被拔除、分辨率变小导致完全落在屏幕外）——后者是正常场景，
/// 调用方应静默回退默认布局，但日志里要能区分，避免排查时误判。
auto parse_and_validate(const Json &j, const std::string &key) -> std::optional<WindowGeometry> {
    const auto parsed = window_geometry_from_json(j);
    if (!parsed.has_value()) {
        AURORA_LOG_WARN("app", "stored window geometry is malformed; ignoring (key=" + key + ")");
        return std::nullopt;
    }
    if (!is_window_geometry_usable(*parsed)) {
        AURORA_LOG_WARN("app", "stored window geometry is off-screen; falling back to default (key=" + key + ")");
        return std::nullopt;
    }
    return parsed;
}

}  // namespace

auto window_geometry_to_json(const WindowGeometry &g) -> Json {
    return Json{
        {"origin_x", g.origin.x},
        {"origin_y", g.origin.y},
        {"width", g.size.width},
        {"height", g.size.height},
        {"mode", static_cast<int>(g.mode)},
        {"display_id", g.display_id},
    };
}

auto window_geometry_from_json(const Json &j) -> std::optional<WindowGeometry> {
    if (!j.is_object()) {
        return std::nullopt;
    }
    bool ok = true;
    WindowGeometry g;
    g.origin.x = static_cast<float>(number_or(j, "origin_x", 0.0, &ok));
    g.origin.y = static_cast<float>(number_or(j, "origin_y", 0.0, &ok));
    g.size.width = static_cast<float>(number_or(j, "width", 0.0, &ok));
    g.size.height = static_cast<float>(number_or(j, "height", 0.0, &ok));
    g.display_id = static_cast<int>(number_or(j, "display_id", -1.0, &ok));
    const double mode = number_or(j, "mode", 0.0, &ok);
    if (!ok || mode < 0.0 || mode > 3.0) {
        return std::nullopt;  // 字段缺失/类型不符/枚举越界：一律视为格式错误
    }
    g.mode = static_cast<WindowMode>(mode);
    return g;
}

auto is_window_geometry_usable(const WindowGeometry &g, const std::vector<Display> &displays) -> bool {
    if (g.size.width <= 0.0F || g.size.height <= 0.0F) {
        return false;
    }
    const float left = g.origin.x;
    const float top = g.origin.y;
    const float right = g.origin.x + g.size.width;
    const float bottom = g.origin.y + g.size.height;
    for (const Display &d : displays) {
        const Rect &w = d.work_area;
        // 与任一工作区有交集即视为可用（允许部分越界：多屏拼接/任务栏遮挡下不应拒绝恢复）。
        if (left < w.right() && right > w.origin.x && top < w.bottom() && bottom > w.origin.y) {
            return true;
        }
    }
    return false;
}

auto is_window_geometry_usable(const WindowGeometry &g) -> bool {
    return is_window_geometry_usable(g, app::list_displays());
}

auto save_window_geometry(preferences::Preferences &prefs, const std::string &key, const WindowGeometry &g) -> void {
    prefs.set(key, window_geometry_to_json(g));
}

auto load_window_geometry(preferences::Preferences &prefs, const std::string &key)
    -> std::optional<WindowGeometry> {
    if (!prefs.contains(key)) {
        return std::nullopt;
    }
    return parse_and_validate(prefs.get<Json>(key, Json{}), key);
}

auto save_window_geometry(preferences::Preferences::Group group, const std::string &key, const WindowGeometry &g)
    -> void {
    group.set(key, window_geometry_to_json(g));
}

auto load_window_geometry(preferences::Preferences::Group group, const std::string &key)
    -> std::optional<WindowGeometry> {
    if (!group.contains(key)) {
        return std::nullopt;
    }
    return parse_and_validate(group.get<Json>(key, Json{}), key);
}

}  // namespace aurora
