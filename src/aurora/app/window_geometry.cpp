#include "aurora/app/window_geometry.h"

#include <algorithm>
#include <string_view>

#include "aurora/core/json.h"
#include "aurora/core/log.h"

namespace aurora {

namespace {

/// @brief 读取数值字段；键缺失或类型不符时置 `ok=false` 并回退默认值。
auto number_or(const json::Value &j, std::string_view key, double fallback, bool *ok) -> double {
    // 指针路径：非对象 / 键缺失 / 非数值（含 double 不可表示的保真数字）一律视为格式错误。
    const json::Value *field = j.at(key);
    if (field == nullptr) {
        *ok = false;
        return fallback;
    }
    const auto value = field->as_double();
    if (!value.has_value()) {
        *ok = false;
        return fallback;
    }
    return *value;
}

/// @brief 解析存储的几何并做可用性校验；任一环节不通过即返回 `nullopt`，并按原因给出 WARN。
///
/// 两类失败刻意区分：**格式错误**（键缺失/类型不符/枚举越界，通常意味着存储被外部破坏）
/// 与**几何不可用**（显示器被拔除、分辨率变小导致完全落在屏幕外）——后者是正常场景，
/// 调用方应静默回退默认布局，但日志里要能区分，避免排查时误判。
auto parse_and_validate(const json::Value &j, const std::string &key) -> std::optional<WindowGeometry> {
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

auto window_geometry_to_json(const WindowGeometry &g) -> json::Value {
    auto out = json::Value::object();
    out.set("origin_x", g.origin.x);
    out.set("origin_y", g.origin.y);
    out.set("width", g.size.width);
    out.set("height", g.size.height);
    out.set("mode", static_cast<int>(g.mode));
    out.set("display_id", g.display_id);
    return out;
}

auto window_geometry_from_json(const json::Value &j) -> std::optional<WindowGeometry> {
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
    // 与任一工作区有交集即视为可用（允许部分越界：多屏拼接/任务栏遮挡下不应拒绝恢复）。
    return std::ranges::any_of(displays, [&](const Display &d) {
        const Rect &w = d.work_area;
        return left < w.right() && right > w.origin.x && top < w.bottom() && bottom > w.origin.y;
    });
}

auto is_window_geometry_usable(const WindowGeometry &g) -> bool {
    return is_window_geometry_usable(g, app::list_displays());
}

auto save_window_geometry(preferences::Preferences &prefs, const std::string &key, const WindowGeometry &g) -> void {
    prefs.set(key, window_geometry_to_json(g));
}

auto load_window_geometry(preferences::Preferences &prefs, const std::string &key) -> std::optional<WindowGeometry> {
    if (!prefs.contains(key)) {
        return std::nullopt;
    }
    return parse_and_validate(prefs.get<json::Value>(key, json::Value{}), key);
}

auto save_window_geometry(preferences::Preferences::Group group, const std::string &key, const WindowGeometry &g)
    -> void {
    group.set(key, window_geometry_to_json(g));
}

auto load_window_geometry(const preferences::Preferences::Group &group, const std::string &key)
    -> std::optional<WindowGeometry> {
    if (!group.contains(key)) {
        return std::nullopt;
    }
    return parse_and_validate(group.get<json::Value>(key, json::Value{}), key);
}

}  // namespace aurora
