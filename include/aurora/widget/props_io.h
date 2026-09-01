#pragma once

#include <cstdint>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/core/types.h"

#include "nlohmann/json.hpp"

namespace aurora {

/// @brief 序列化所用的 JSON 类型别名（nlohmann/json，vendor 于 third_party）。
using Json = nlohmann::json;

/// @brief 强类型尺寸意图 → JSON：["px",v] / ["percent",v] / "fill" / "auto"。
[[nodiscard]] inline auto length_to_json(const Length &len) -> Json {
    switch (len.kind) {
    case LengthKind::WrapContent: return { "auto" };
    case LengthKind::Expand: return { "fill" };
    case LengthKind::Fixed: {
        Json a = Json::array();
        a.push_back("px");
        a.push_back(len.value);
        return a;
    }
    case LengthKind::Fraction: {
        Json a = Json::array();
        a.push_back("percent");
        a.push_back(len.value);
        return a;
    }
    }
    return { "auto" };
}

/// @brief JSON → 强类型尺寸意图（解析 lengthToJson 的输出）。
[[nodiscard]] inline auto json_to_length(const Json &j) -> Length {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "auto") {
            return Length::wrap();
        }
        if (s == "fill") {
            return Length::expand();
        }
        return Length::wrap();
    }
    if (j.is_array() && j.size() == 2) {
        const std::string kind = j[0].get<std::string>();
        const float v = j[1].get<float>();
        if (kind == "px") {
            return Length::fixed(v);
        }
        if (kind == "percent") {
            return Length::ratio(v);
        }
    }
    return Length::wrap();
}

/// @brief 颜色 → JSON：[r,g,b,a]（0-255）。
[[nodiscard]] inline auto color_to_json(const Color &c) -> Json {
    Json a = Json::array();
    a.push_back(c.m_r);
    a.push_back(c.m_g);
    a.push_back(c.m_b);
    a.push_back(c.m_a);
    return a;
}

/// @brief JSON → 颜色（解析 colorToJson 的输出；格式不符回退黑色）。
[[nodiscard]] inline auto json_to_color(const Json &j) -> Color {
    if (j.is_array() && j.size() >= 4) {
        return Color{ static_cast<std::uint8_t>(j[0].get<int>()), static_cast<std::uint8_t>(j[1].get<int>()),
                      static_cast<std::uint8_t>(j[2].get<int>()), static_cast<std::uint8_t>(j[3].get<int>()) };
    }
    return Color::black();
}

/// @brief EdgeInsets -> JSON 对象 {left,top,right,bottom}。
[[nodiscard]] inline auto edge_insets_to_json(const EdgeInsets &e) -> Json {
    Json o;
    o["left"] = e.left;
    o["top"] = e.top;
    o["right"] = e.right;
    o["bottom"] = e.bottom;
    return o;
}

/// @brief JSON -> EdgeInsets（解析 edge_insets_to_json 输出；缺字段回退 0）。
[[nodiscard]] inline auto json_to_edge_insets(const Json &j) -> EdgeInsets {
    EdgeInsets e{};
    if (j.is_object()) {
        if (j.contains("left")) {
            e.left = j["left"].get<float>();
        }
        if (j.contains("top")) {
            e.top = j["top"].get<float>();
        }
        if (j.contains("right")) {
            e.right = j["right"].get<float>();
        }
        if (j.contains("bottom")) {
            e.bottom = j["bottom"].get<float>();
        }
    }
    return e;
}

// ---------- 共享枚举 <-> JSON（参考 Flutter 命名） ----------

/// @brief TextAlign -> JSON 字符串。
[[nodiscard]] inline auto text_align_to_json(TextAlign v) -> Json {
    switch (v) {
    case TextAlign::Left: return "Left";
    case TextAlign::Right: return "Right";
    case TextAlign::Center: return "Center";
    case TextAlign::Start: return "Start";
    case TextAlign::End: return "End";
    case TextAlign::Justify: return "Justify";
    }
    return "Left";
}

/// @brief JSON -> TextAlign（未知值回退 Left）。
[[nodiscard]] inline auto json_to_text_align(const Json &j) -> TextAlign {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Left") {
            return TextAlign::Left;
        }
        if (s == "Right") {
            return TextAlign::Right;
        }
        if (s == "Center") {
            return TextAlign::Center;
        }
        if (s == "Start") {
            return TextAlign::Start;
        }
        if (s == "End") {
            return TextAlign::End;
        }
        if (s == "Justify") {
            return TextAlign::Justify;
        }
    }
    return TextAlign::Left;
}

/// @brief TextOverflow -> JSON 字符串。
[[nodiscard]] inline auto text_overflow_to_json(TextOverflow v) -> Json {
    switch (v) {
    case TextOverflow::Clip: return "Clip";
    case TextOverflow::Ellipsis: return "Ellipsis";
    case TextOverflow::Fade: return "Fade";
    }
    return "Clip";
}

/// @brief JSON -> TextOverflow（未知值回退 Clip）。
[[nodiscard]] inline auto json_to_text_overflow(const Json &j) -> TextOverflow {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Clip") {
            return TextOverflow::Clip;
        }
        if (s == "Ellipsis") {
            return TextOverflow::Ellipsis;
        }
        if (s == "Fade") {
            return TextOverflow::Fade;
        }
    }
    return TextOverflow::Clip;
}

/// @brief FontWeight -> JSON（数值字符串，如 "700"）。
[[nodiscard]] inline auto font_weight_to_json(FontWeight v) -> Json { return std::to_string(static_cast<int>(v)); }

/// @brief JSON -> FontWeight（按数值匹配，未知回退 Normal）。
/// @note 字符串经 Inspector PUT / JSON 文件加载等不可信通道进入，非法数值串
///       （如 "bold"）此前裸调 `std::stoi()` 抛 `std::invalid_argument` 可致崩溃，现回退默认。
[[nodiscard]] inline auto json_to_font_weight(const Json &j) -> FontWeight {
    int w = 400;
    if (j.is_string()) {
        try {
            w = std::stoi(j.get<std::string>());
        } catch (...) {
            w = 400;
        }
    } else if (j.is_number()) {
        w = j.get<int>();
    }
    switch (w) {
    case 100: return FontWeight::Thin;
    case 200: return FontWeight::ExtraLight;
    case 300: return FontWeight::Light;
    case 400: return FontWeight::Normal;
    case 500: return FontWeight::Medium;
    case 600: return FontWeight::SemiBold;
    case 700: return FontWeight::Bold;
    case 800: return FontWeight::ExtraBold;
    case 900: return FontWeight::Black;
    default: return FontWeight::Normal;
    }
}

/// @brief FontStyle -> JSON 字符串。
[[nodiscard]] inline auto font_style_to_json(FontStyle v) -> Json {
    return v == FontStyle::Italic ? Json("Italic") : Json("Normal");
}

/// @brief JSON -> FontStyle（未知值回退 Normal）。
[[nodiscard]] inline auto json_to_font_style(const Json &j) -> FontStyle {
    if (j.is_string() && j.get<std::string>() == "Italic") {
        return FontStyle::Italic;
    }
    return FontStyle::Normal;
}

/// @brief TextDecoration -> JSON（按位组合序列化为启用的字符串数组）。
[[nodiscard]] inline auto text_decoration_to_json(TextDecoration v) -> Json {
    Json a = Json::array();
    if (v == TextDecoration::None) {
        a.push_back("None");
        return a;
    }
    if (decoration_has(v, TextDecoration::Underline)) {
        a.push_back("Underline");
    }
    if (decoration_has(v, TextDecoration::Overline)) {
        a.push_back("Overline");
    }
    if (decoration_has(v, TextDecoration::LineThrough)) {
        a.push_back("LineThrough");
    }
    return a;
}

/// @brief JSON -> TextDecoration（接受字符串数组或单个字符串；未知项忽略）。
[[nodiscard]] inline auto json_to_text_decoration(const Json &j) -> TextDecoration {
    auto result = TextDecoration::None;
    auto add = [&](const std::string &s) -> void {
        if (s == "None") {
            result = TextDecoration::None;
        } else if (s == "Underline") {
            result |= TextDecoration::Underline;
        } else if (s == "Overline") {
            result |= TextDecoration::Overline;
        } else if (s == "LineThrough") {
            result |= TextDecoration::LineThrough;
        }
    };
    if (j.is_array()) {
        for (const auto &item : j) {
            if (item.is_string()) {
                add(item.get<std::string>());
            }
        }
    } else if (j.is_string()) {
        add(j.get<std::string>());
    }
    return result;
}

/// @brief MainAxisSize -> JSON 字符串。
[[nodiscard]] inline auto main_axis_size_to_json(MainAxisSize v) -> Json {
    return v == MainAxisSize::Max ? Json("Max") : Json("Min");
}

/// @brief JSON -> MainAxisSize（未知值回退 Min）。
[[nodiscard]] inline auto json_to_main_axis_size(const Json &j) -> MainAxisSize {
    if (j.is_string() && j.get<std::string>() == "Max") {
        return MainAxisSize::Max;
    }
    return MainAxisSize::Min;
}

/// @brief MainAxisAlignment -> JSON 字符串。
[[nodiscard]] inline auto main_axis_alignment_to_json(MainAxisAlignment v) -> Json {
    switch (v) {
    case MainAxisAlignment::Start: return "Start";
    case MainAxisAlignment::Center: return "Center";
    case MainAxisAlignment::End: return "End";
    case MainAxisAlignment::SpaceBetween: return "SpaceBetween";
    case MainAxisAlignment::SpaceAround: return "SpaceAround";
    case MainAxisAlignment::SpaceEvenly: return "SpaceEvenly";
    }
    return "Start";
}

/// @brief JSON -> MainAxisAlignment（未知值回退 Start）。
[[nodiscard]] inline auto json_to_main_axis_alignment(const Json &j) -> MainAxisAlignment {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Center") {
            return MainAxisAlignment::Center;
        }
        if (s == "End") {
            return MainAxisAlignment::End;
        }
        if (s == "SpaceBetween") {
            return MainAxisAlignment::SpaceBetween;
        }
        if (s == "SpaceAround") {
            return MainAxisAlignment::SpaceAround;
        }
        if (s == "SpaceEvenly") {
            return MainAxisAlignment::SpaceEvenly;
        }
    }
    return MainAxisAlignment::Start;
}

/// @brief CrossAxisAlignment -> JSON 字符串。
[[nodiscard]] inline auto cross_axis_alignment_to_json(CrossAxisAlignment v) -> Json {
    switch (v) {
    case CrossAxisAlignment::Start: return "Start";
    case CrossAxisAlignment::Center: return "Center";
    case CrossAxisAlignment::End: return "End";
    case CrossAxisAlignment::Stretch: return "Stretch";
    }
    return "Start";
}

/// @brief JSON -> CrossAxisAlignment（未知值回退 Start）。
[[nodiscard]] inline auto json_to_cross_axis_alignment(const Json &j) -> CrossAxisAlignment {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Center") {
            return CrossAxisAlignment::Center;
        }
        if (s == "End") {
            return CrossAxisAlignment::End;
        }
        if (s == "Stretch") {
            return CrossAxisAlignment::Stretch;
        }
    }
    return CrossAxisAlignment::Start;
}

/// @brief StackFit -> JSON 字符串。
[[nodiscard]] inline auto stack_fit_to_json(StackFit v) -> Json {
    switch (v) {
    case StackFit::Loose: return "Loose";
    case StackFit::Expand: return "Expand";
    case StackFit::Passthrough: return "Passthrough";
    }
    return "Loose";
}

/// @brief JSON -> StackFit（未知值回退 Loose）。
[[nodiscard]] inline auto json_to_stack_fit(const Json &j) -> StackFit {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Loose") {
            return StackFit::Loose;
        }
        if (s == "Expand") {
            return StackFit::Expand;
        }
        if (s == "Passthrough") {
            return StackFit::Passthrough;
        }
    }
    return StackFit::Loose;
}

/// @brief BoxFit -> JSON 字符串。
[[nodiscard]] inline auto box_fit_to_json(BoxFit v) -> Json {
    switch (v) {
    case BoxFit::Fill: return "Fill";
    case BoxFit::Contain: return "Contain";
    case BoxFit::Cover: return "Cover";
    case BoxFit::FitWidth: return "FitWidth";
    case BoxFit::FitHeight: return "FitHeight";
    case BoxFit::None: return "None";
    case BoxFit::ScaleDown: return "ScaleDown";
    }
    return "Fill";
}

/// @brief JSON -> BoxFit（未知值回退 Fill）。
[[nodiscard]] inline auto json_to_box_fit(const Json &j) -> BoxFit {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Fill") {
            return BoxFit::Fill;
        }
        if (s == "Contain") {
            return BoxFit::Contain;
        }
        if (s == "Cover") {
            return BoxFit::Cover;
        }
        if (s == "FitWidth") {
            return BoxFit::FitWidth;
        }
        if (s == "FitHeight") {
            return BoxFit::FitHeight;
        }
        if (s == "None") {
            return BoxFit::None;
        }
        if (s == "ScaleDown") {
            return BoxFit::ScaleDown;
        }
    }
    return BoxFit::Fill;
}

/// @brief OverflowStrategy -> JSON 字符串。
[[nodiscard]] inline auto overflow_strategy_to_json(OverflowStrategy v) -> Json {
    switch (v) {
    case OverflowStrategy::Visible: return "Visible";
    case OverflowStrategy::Hidden: return "Hidden";
    case OverflowStrategy::Clip: return "Clip";
    case OverflowStrategy::Scroll: return "Scroll";
    }
    return "Visible";
}

/// @brief JSON -> OverflowStrategy（未知值回退 Visible）。
[[nodiscard]] inline auto json_to_overflow_strategy(const Json &j) -> OverflowStrategy {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "Visible") {
            return OverflowStrategy::Visible;
        }
        if (s == "Hidden") {
            return OverflowStrategy::Hidden;
        }
        if (s == "Clip") {
            return OverflowStrategy::Clip;
        }
        if (s == "Scroll") {
            return OverflowStrategy::Scroll;
        }
    }
    return OverflowStrategy::Visible;
}

} // namespace aurora

// ---- validate_prop<T>：属性值约束验证（specification/04-widget.md §2.2） ----
// validate_prop 模板特化定义在 descriptor.h（需要 PropDescriptor 完整定义，
// 而 descriptor.h 已 include 本头文件，避免循环依赖）。
// validate_enum_string 辅助函数同样在 descriptor.h。
