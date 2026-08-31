#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "aurora/widget/serialization.h"

namespace aurora::serialization {

/// @brief 代码生成风格（规格 #22 / §I）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
enum class CodeStyle : std::uint8_t {
    Fluent,         // 扁平/链式：au::Column{ a, b }; au::Button{ .label = "OK" }
    StepByStep,     // 分步赋值：auto btn = au::Button{}; btn.label = "OK";
    DesignatedInit, // 指定初始化器：au::Button{ .label = "OK", .children = {...} }
};

namespace detail {
/// 多子扁平容器（直接罗列子项，免 Props 包裹）。
[[nodiscard]] constexpr auto is_flat_container(std::string_view type) noexcept -> bool {
    constexpr std::array<std::string_view, 6> k_flat = { "Column", "Row", "Stack", "Grid", "Scroll", "Card" };
    return std::ranges::find(k_flat, type) != k_flat.end();
}

/// @brief 把序列化 type 名映射到 C++ 类名（部分控件类名与 type 名不同）。
[[nodiscard]] inline auto cpp_class(std::string_view type) -> std::string {
    if (type == "Image") {
        return "ImageView";
    }
    return std::string{ type };
}
/// @brief 把序列化 type 名映射到其 Props 聚合类型名。
[[nodiscard]] inline auto cpp_props(std::string_view type) -> std::string {
    if (type == "Image") {
        return "ImageViewProps";
    }
    return std::string{ type } + std::string{ "Props" };
}

// ---------- emit_props 辅助：枚举映射 + 值分派 ----------

/// @brief JSON 属性名 → C++ 枚举类型名（用于 codegen 输出 EnumType::Value）。
/// @note 键必须同时覆盖「库自描述里真实出现的属性名」（`to_json` 产出即此名，如 Stack 的
///       `alignment`、Text 的 `overflow`、Drawer 的 `side`、ToastHost 的 `position`）与历史
///       合成名（`text_overflow` / `stack_fit` / `box_fit` / `overflow_strategy`，供手工 JSON 使用）：
///       缺前者会让真实 UI 树的枚举属性退化成裸字符串，生成的代码编译不过。
/// @note `fit` 与 `orientation` **不能**按键消歧——`fit` 在 Stack 上是 `StackFit`、在
///       VideoPlayer 上是 `BoxFit`；`orientation` 在 Divider 上是 `Orientation`、在 Splitter 上是
///       `SplitterOrientation`（两者取值集同名）。要正确输出必须按 `prop_descriptors[].type`
///       分派（需把属性声明类型透传进 emit_prop_value），故此处刻意不登记，避免猜错类型。
[[nodiscard]] inline auto enum_type_for_key(const std::string &key) -> std::string {
    static const std::unordered_map<std::string, std::string> m = {
        { "text_align", "TextAlign" },
        { "text_overflow", "TextOverflow" },
        { "overflow", "TextOverflow" },     // Text 的真实属性名
        { "decoration", "TextDecoration" }, // Text 的真实属性名（字符串形态；数组形态另处理）
        { "main_axis_alignment", "MainAxisAlignment" },
        { "cross_axis_alignment", "CrossAxisAlignment" },
        { "main_axis_size", "MainAxisSize" },
        { "stack_fit", "StackFit" },
        { "alignment", "Alignment" }, // Stack 的真实属性名
        { "box_fit", "BoxFit" },
        { "overflow_strategy", "OverflowStrategy" },
        { "font_style", "FontStyle" },
        { "side", "DrawerSide" },        // Drawer 的真实属性名
        { "position", "ToastPosition" }, // ToastHost 的真实属性名
    };
    const auto it = m.find(key);
    return it != m.end() ? it->second : std::string{};
}

/// @brief 把 PascalCase 枚举值字符串转为 codegen 用的 EnumType::Value 表达式。
[[nodiscard]] inline auto emit_enum_value(std::string_view enum_type, std::string_view json_val) -> std::string {
    return std::string{ enum_type } + std::string{ "::" } + std::string{ json_val };
}

/// @brief FontWeight 特殊处理：JSON 存储为数值字符串 "100".."900"。
[[nodiscard]] inline auto emit_font_weight(const Json &value) -> std::string_view {
    int w = 400;
    if (value.is_string()) {
        const auto &s = value.get_ref<const std::string &>();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) std::from_chars 需要首尾指针
        std::from_chars(s.data(), s.data() + s.size(), w);
    } else if (value.is_number()) {
        w = value.get<int>();
    }
    switch (w) {
    case 100: return "FontWeight::Thin";
    case 200: return "FontWeight::ExtraLight";
    case 300: return "FontWeight::Light";
    case 400: return "FontWeight::Normal";
    case 500: return "FontWeight::Medium";
    case 600: return "FontWeight::SemiBold";
    case 700: return "FontWeight::Bold";
    case 800: return "FontWeight::ExtraBold";
    case 900: return "FontWeight::Black";
    default: return "FontWeight::Normal";
    }
}

/// @brief TextDecoration 特殊处理：JSON 存储为字符串数组 ["Underline", ...]。
[[nodiscard]] inline auto emit_text_decoration(const Json &value) // NOLINT
    -> std::string {
    if (value.is_array()) {
        std::string result{};
        for (const auto &item : value) {
            if (!item.is_string()) {
                continue;
            }
            const std::string s = item.get<std::string>();
            if (s == "None") {
                return "TextDecoration::None";
            }
            if (!result.empty()) {
                result += " | ";
            }
            if (s == "Underline") {
                result += "TextDecoration::Underline";
            } else if (s == "Overline") {
                result += "TextDecoration::Overline";
            } else if (s == "LineThrough") {
                result += "TextDecoration::LineThrough";
            }
        }
        return result.empty() ? "TextDecoration::None" : result;
    }
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "None") {
            return "TextDecoration::None";
        }
        if (s == "Underline") {
            return "TextDecoration::Underline";
        }
        if (s == "Overline") {
            return "TextDecoration::Overline";
        }
        if (s == "LineThrough") {
            return "TextDecoration::LineThrough";
        }
    }
    return "TextDecoration::None";
}

/// @brief 转义字符串中的 C++ 特殊字符（"、\\、换行等）。
[[nodiscard]] inline auto escape_cpp_string(std::string_view s) -> std::string {
    std::string out{};
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

/// @brief 智能分派：根据 key 名称和 value 的 JSON 类型生成 C++ 表达式。
[[nodiscard]] inline auto emit_prop_value(const std::string &key, const Json &value) -> std::string { // NOLINT
    // --- 枚举属性（string → EnumType::Value）---
    if (value.is_string()) {
        const std::string enum_type = enum_type_for_key(key);
        if (!enum_type.empty()) {
            return emit_enum_value(enum_type, value.get<std::string>());
        }
        // FontWeight: 数值字符串 → FontWeight::Bold 等
        if (key == "font_weight") {
            return std::string(emit_font_weight(value));
        }
        // Length 特殊字符串
        if (value.get<std::string>() == "auto") {
            return "au::auto_length()";
        }
        if (value.get<std::string>() == "fill") {
            return "au::fill()";
        }
        // 普通字符串
        return "\"" + escape_cpp_string(value.get<std::string>()) + "\"";
    }
    // --- bool ---
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    // --- number ---
    if (value.is_number()) {
        if (value.is_number_float()) {
            std::ostringstream ss{};
            ss << value.get<float>() << 'f';
            return ss.str();
        }
        return std::to_string(value.get<int>());
    }
    // --- array: Length ["px"/"percent", N] | Color [r,g,b,a] | TextDecoration [...] ---
    if (value.is_array()) {
        if (value.size() == 2 && value[0].is_string()) {
            const std::string unit = value[0].get<std::string>();
            if (unit == "px") {
                return std::string{ "au::px(" } + std::to_string(value[1].get<float>()) + std::string{ ")" };
            }
            if (unit == "percent") {
                return std::string{ "au::percent(" } + std::to_string(value[1].get<float>()) + std::string{ ")" };
            }
        }
        if (value.size() >= 4 && value[0].is_number()) {
            return std::string{ "Color{" } + std::to_string(value[0].get<int>()) + std::string{ "," } +
                   std::to_string(value[1].get<int>()) + std::string{ "," } + std::to_string(value[2].get<int>()) +
                   std::string{ "," } + std::to_string(value[3].get<int>()) + std::string{ "}" };
        }
        if (key == "text_decoration") {
            return emit_text_decoration(value);
        }
    }
    // --- object: EdgeInsets {top,right,bottom,left} | legacy Length {value,unit} ---
    if (value.is_object()) {
        if (value.contains("top") && value.contains("left") && value.contains("right") && value.contains("bottom")) {
            return std::string{ "EdgeInsets{" } + std::to_string(value["top"].get<float>()) + std::string{ "," } +
                   std::to_string(value["right"].get<float>()) + std::string{ "," } +
                   std::to_string(value["bottom"].get<float>()) + std::string{ "," } +
                   std::to_string(value["left"].get<float>()) + std::string{ "}" };
        }
        if (value.contains("value") && value.contains("unit")) {
            const auto unit = value["unit"].get<std::string>();
            const auto v = value["value"].get<float>();
            if (unit == "pct") {
                return std::string{ "au::percent(" } + std::to_string(v) + std::string{ ")" };
            }
            return std::string{ "au::px(" } + std::to_string(v) + std::string{ ")" };
        }
    }
    // --- fallback ---
    return "/* unknown */";
}

/// 把已暴露的属性渲染为 "prefix + key = expr" 片段，覆盖全部 props_io 类型。
[[nodiscard]] inline auto emit_props(const Json &props, const char *prefix) -> std::string {
    std::string out{};
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string &key = it.key();
        const Json &value = it.value();
        auto expr = emit_prop_value(key, value);
        if (expr == "/* unknown */") {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += prefix;
        out += key;
        out += " = ";
        out += expr;
    }
    return out;
}
} // namespace detail

/// @brief 生成 "au::Type{ ... }" 形式（指定初始化器），indent 为缩进空格数。
[[nodiscard]] inline auto to_code_di(const Json &node, int indent) -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    const std::string pad2 = pad + std::string{ "    " };
    const std::string type = node.contains("type") ? node["type"].get<std::string>() : "Column";
    const Json &props = node.contains("props") ? node["props"] : Json::object();
    const bool has_children = node.contains("children") && node["children"].is_array() && !node["children"].empty();
    std::ostringstream os{};
    os << "au::" << detail::cpp_class(type) << "{";
    const std::string p = detail::emit_props(props, ".");
    if (!p.empty()) {
        os << p;
    }
    if (has_children) {
        if (!p.empty()) {
            os << ", ";
        }
        os << ".children = {\n";
        const Json &kids = node["children"];
        for (std::size_t i = 0; i < kids.size(); ++i) {
            os << pad2 << to_code_di(kids[i], indent + 2);
            if (i + 1 < kids.size()) {
                os << ",";
            }
            os << "\n";
        }
        os << pad << "}";
    }
    os << "}";
    return os.str();
}

/// @brief 分步赋值形式：auto w = au::Type{}; w.prop = val; 返回变量名并追加语句到 os。
[[nodiscard]] inline auto to_code_sb(const Json &node, int indent, std::ostringstream &os, int &counter)
    -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    const std::string type = node.contains("type") ? node["type"].get<std::string>() : "Column";
    const Json &props = node.contains("props") ? node["props"] : Json::object();
    const bool has_children = node.contains("children") && node["children"].is_array() && !node["children"].empty();
    const std::string var = std::string{ "__w" } + std::to_string(counter++);
    os << pad << "auto " << var << " = au::" << detail::cpp_class(type) << "{};\n";
    auto assign = detail::emit_props(props, ".");
    if (!assign.empty()) {
        std::string::size_type pos = 0;
        while ((pos = assign.find('.', pos)) != std::string::npos) {
            assign.replace(pos, 1, var + std::string{ "." });
            pos += var.size() + 1;
        }
        os << pad << assign << ";\n";
    }
    if (has_children) {
        const bool flat = detail::is_flat_container(type);
        os << pad << var << (flat ? ".children" : "") << " = { ";
        const Json &kids = node["children"];
        for (std::size_t i = 0; i < kids.size(); ++i) {
            os << to_code_sb(kids[i], indent + 1, os, counter);
            if (i + 1 < kids.size()) {
                os << ", ";
            }
        }
        os << " };\n";
    }
    return var;
}

/**
 * @brief 把序列化的 widget 树 JSON 反向生成为 Aurora C++ 源码（需求 #22 / 规格 §4.1.1）。
 *
 * 输入为 `to_json(widget)` 产生的结构快照。默认 Fluent 风格（扁平容器 + Type(Props{...}) 叶形式）。
 * 可用 CodeStyle 选择 DesignatedInit（统一指定初始化器）或 StepByStep（分步赋值）。
 *
 * 用于「设计工具 → 代码」工作流，或把快照作为可编辑源码。
 */
[[nodiscard]] inline auto to_code(const Json &node, int indent = 0) -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
    const std::string pad2 = pad + std::string{ "    " };
    const std::string type = node.contains("type") ? node["type"].get<std::string>() : "Column";
    const Json &props = node.contains("props") ? node["props"] : Json::object();
    const bool has_children = node.contains("children") && node["children"].is_array() && !node["children"].empty();

    if (has_children && detail::is_flat_container(type)) {
        std::ostringstream os{};
        os << "au::" << detail::cpp_class(type) << "{";
        const Json &kids = node["children"];
        for (std::size_t i = 0; i < kids.size(); ++i) {
            os << "\n" << pad2 << to_code(kids[i], indent + 2);
            if (i + 1 < kids.size()) {
                os << ",";
            }
        }
        if (type == "Grid" && props.contains("columns") && props["columns"].get<int>() > 1) {
            os << ",\n" << pad2 << std::to_string(props["columns"].get<int>());
        }
        os << "\n" << pad << "}";
        return os.str();
    }

    std::ostringstream os{};
    os << "au::" << detail::cpp_class(type) << "(au::" << detail::cpp_props(type) << "{";
    const std::string props_str = detail::emit_props(props, ".");
    if (!props_str.empty()) {
        os << props_str;
    }
    if (has_children) {
        if (!props_str.empty()) {
            os << ", ";
        }
        os << ".children = {\n";
        const Json &kids = node["children"];
        for (std::size_t i = 0; i < kids.size(); ++i) {
            os << pad2 << to_code(kids[i], indent + 2);
            if (i + 1 < kids.size()) {
                os << ",";
            }
            os << "\n";
        }
        os << pad << "}";
    }
    os << "})";
    return os.str();
}

/// @brief 按指定风格生成代码（规格 #22）。默认 Fluent，与 to_code(node, int) 行为一致。
[[nodiscard]] inline auto to_code(const Json &node, CodeStyle style, int indent = 0) -> std::string {
    switch (style) {
    case CodeStyle::DesignatedInit: return to_code_di(node, indent);
    case CodeStyle::StepByStep: {
        std::ostringstream os{};
        int counter = 0;
        (void)to_code_sb(node, indent, os, counter);
        return os.str();
    }
    case CodeStyle::Fluent:
    default: return to_code(node, indent);
    }
}

/// @brief 便捷：直接对 widget 取快照再生成代码。
[[nodiscard]] inline auto to_code(const Widget &w) -> std::string { return to_code(to_json(w)); }

} // namespace aurora::serialization
