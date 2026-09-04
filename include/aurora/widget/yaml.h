#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "nlohmann/json.hpp"

namespace aurora {

using Json = nlohmann::json;

namespace serialization {

namespace detail {

/// @brief 判断字符串是否需要 YAML 引号包裹（会被 YAML 解析器解析为非字符串类型）。
[[nodiscard]] inline auto yaml_needs_quoting(const std::string &s) -> bool {
    if (s.empty()) {
        return true;
    }

    // YAML 保留字 / 布尔字面量
    if (s == "true" || s == "false" || s == "yes" || s == "no" || s == "on" || s == "off" || s == "null" ||
        s == "True" || s == "False" || s == "Yes" || s == "No" || s == "On" || s == "Off" || s == "NULL" ||
        s == "TRUE" || s == "FALSE" || s == "YES" || s == "NO" || s == "ON" || s == "OFF" || s == "~") {
        return true;
    }

    // 检测是否为整数（可选正负号 + 数字）
    {
        std::size_t i = 0;
        if (s[i] == '+' || s[i] == '-') {
            ++i;
        }
        if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0)) {
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0)) {
                ++i;
            }
            if (i == s.size()) {
                return true;  // 纯整数
            }
        }
    }

    // 检测是否为浮点数（含小数点或科学计数法）
    {
        const char *begin = s.c_str();
        char *end = nullptr;
        (void)std::strtod(begin, &end);
        if (end != nullptr && static_cast<std::size_t>(end - begin) == s.size()) {
            return true;  // 整个字符串被解析为数字
        }
    }

    // 含 YAML 特殊字符
    static const std::string SPECIAL = ":#{}[],&*?|-<>=!%@`\\\"";
    for (const char c : s) {
        if (SPECIAL.find(c) != std::string::npos) {
            return true;
        }
    }

    // 前后空白
    return (std::isspace(static_cast<unsigned char>(s.front())) != 0) ||
           (std::isspace(static_cast<unsigned char>(s.back())) != 0);
}

/// @brief 将字符串用双引号包裹并转义内部特殊字符。
[[nodiscard]] inline auto yaml_quote_string(const std::string &s) -> std::string {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    out += '"';
    return out;
}

// 前向声明：容器辅助函数需要递归回调主分发器。
[[nodiscard]] inline auto yaml_emit(const Json &j, int indent) -> std::string;

/// @brief 将 JSON 字符串值转为 YAML 字符串（按需加引号）。
[[nodiscard]] inline auto emit_string(const Json &j) -> std::string {
    auto s = j.get<std::string>();
    return yaml_needs_quoting(s) ? yaml_quote_string(s) : std::move(s);
}

/// @brief 将 JSON 浮点值转为 YAML 浮点字符串。
[[nodiscard]] inline auto emit_float(double v) -> std::string {
    if (std::isnan(v)) {
        return ".nan";
    }
    if (std::isinf(v)) {
        return v > 0 ? ".inf" : "-.inf";
    }
    std::ostringstream os;
    os << v;
    std::string s = os.str();
    // 确保有小数点（YAML 浮点要求）
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
}

/// @brief 将 JSON 标量（null / bool / number / string）转为 YAML 字符串。
[[nodiscard]] inline auto emit_scalar(const Json &j) -> std::string {
    if (j.is_null()) {
        return "null";
    }
    if (j.is_boolean()) {
        return j.get<bool>() ? "true" : "false";
    }
    if (j.is_number_integer()) {
        return std::to_string(j.get<std::int64_t>());
    }
    if (j.is_number_unsigned()) {
        return std::to_string(j.get<std::uint64_t>());
    }
    if (j.is_number_float()) {
        return emit_float(j.get<double>());
    }
    if (j.is_string()) {
        return emit_string(j);
    }
    return "null";  // fallback
}

/// @brief 将 JSON 数组转为 YAML 数组字符串。
[[nodiscard]] inline auto emit_array(const Json &j, int indent, const std::string &pad) -> std::string {
    if (j.empty()) {
        return "[]";
    }
    std::ostringstream os;
    for (std::size_t i = 0; i < j.size(); ++i) {
        if (i > 0) {
            os << '\n';
        }
        os << pad << "- " << yaml_emit(j[i], indent + 1);
    }
    return os.str();
}

/// @brief 将 JSON 对象的一个键值对转为 YAML 行块。
[[nodiscard]] inline auto emit_object_value(const Json &val, const std::string &key, int indent, const std::string &pad)
    -> std::string {
    std::ostringstream os;
    const std::string yaml_key = yaml_needs_quoting(key) ? yaml_quote_string(key) : key;
    if (val.is_object() || val.is_array()) {
        const std::string child = yaml_emit(val, indent + 1);
        if (val.is_array() && !val.empty() && !val[0].is_object()) {
            // 简单标量数组：- item 直接跟在 key 后面
            os << pad << yaml_key << ":\n" << child;
        } else if (val.is_array() && !val.empty()) {
            // 对象数组：第一个 - 与 key 同行
            os << pad << yaml_key << ":\n";
            const std::string inner_pad(static_cast<std::size_t>(indent + 1) * 2, ' ');
            for (std::size_t i = 0; i < val.size(); ++i) {
                if (i > 0) {
                    os << '\n';
                }
                os << inner_pad << "- " << yaml_emit(val[i], indent + 2);
            }
        } else {
            // 空数组或空对象
            os << pad << yaml_key << ": " << child;
        }
    } else {
        os << pad << yaml_key << ": " << yaml_emit(val, 0);
    }
    return os.str();
}

/// @brief 将 JSON 对象转为 YAML 对象字符串。
[[nodiscard]] inline auto emit_object(const Json &j, int indent, const std::string &pad) -> std::string {
    if (j.empty()) {
        return "{}";
    }
    std::ostringstream os;
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!first) {
            os << '\n';
        }
        first = false;
        os << emit_object_value(it.value(), it.key(), indent, pad);
    }
    return os.str();
}

/// @brief 递归下降 YAML 发射器，将 nlohmann::json 转为 YAML 字符串。
[[nodiscard]] inline auto yaml_emit(const Json &j, int indent) -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');

    if (j.is_object()) {
        return emit_object(j, indent, pad);
    }
    if (j.is_array()) {
        return emit_array(j, indent, pad);
    }
    return emit_scalar(j);
}

}  // namespace detail

/// @brief 将 JSON 值转换为 YAML 格式字符串（2 空格缩进）。
[[nodiscard]] inline auto to_yaml(const Json &j, int indent = 0) -> std::string { return detail::yaml_emit(j, indent); }

}  // namespace serialization
}  // namespace aurora
