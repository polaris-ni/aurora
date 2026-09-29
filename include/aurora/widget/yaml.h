#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "aurora/core/json.h"

namespace aurora {

/// @brief JSON 内存 DOM 类型别名（aurora::json::Value），序列化与反序列化 API 的统一载体。
using Json = json::Value;

namespace serialization {

namespace detail {

/// @brief 判断字符串是否需要 YAML 引号包裹（会被 YAML 解析器解析为非字符串类型）。
/// @param s 待检查的原始字符串。
/// @return 空串、命中 YAML 保留字/布尔字面量、可整体解析为整数或浮点数、含 YAML 特殊字符或首尾含空白时为 true。
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
    // 惰性构造的函数内 static：字符串字面量构造 `std::string` 需分配，常量初始化不可能；而首建
    // 时刻与跨 TU 静态初始化顺序无关（本检查的担心面）。仅浏览器口径命中——native 遍同一份代码
    // 不报（CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
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
/// @param s 待引用的原始字符串。
/// @return 双引号包裹的串，内部 " \ 及换行/回车/制表符已转为 YAML 转义序列。
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

/// @brief 递归下降 YAML 发射器，将 JSON 值（`aurora::json::Value`）转为 YAML 字符串。
/// 顶层空容器内联为 `{}` / `[]`；非空容器一律走块形态，空容器只在「值位置」由调用方内联。
/// 容器辅助函数 emit_object / emit_array 递归回调本主分发器，故在此先行声明，定义见下文。
/// @param j 待发射的任意 JSON 值。
/// @param indent 起始缩进层级（一级 = 2 空格列）。
/// @return 该值对应的 YAML 文本（对象/数组块、标量字面量或内联空容器）。
[[nodiscard]] inline auto yaml_emit(const Json &j, int indent) -> std::string;
/// @brief 将 JSON 对象转为 YAML 对象块：逐键输出键行，每行前缀 pad（空对象由调用方内联为 `{}`，不进此处）。
/// 数组项可为对象、对象值可为数组，与 emit_array 互递归，定义见下文。
/// @param j 待发射的 JSON 对象（非空）。
/// @param indent 缩进层级（键值下沉一级）。
/// @param pad 本层级每行前缀。
/// @return 每键一行的 YAML 对象块（行间以换行分隔）。
[[nodiscard]] inline auto emit_object(const Json &j, int indent, const std::string &pad) -> std::string;
/// @brief 将 JSON 数组转为 YAML 块：每项以 `- ` 起一行，行首缩进为 indent 级（空数组由调用方内联为 `[]`，不进此处）。
/// 与 emit_object 互递归，定义见下文。
/// @param j 待发射的 JSON 数组（非空）。
/// @param indent 缩进层级（子项下沉一级）。
/// @param pad 本层级每行前缀。
/// @return 每项一行的 YAML 数组块；对象/数组子项的块首行缩进让给 `- ` 前缀，标量子项内联。
[[nodiscard]] inline auto emit_array(const Json &j, int indent, const std::string &pad) -> std::string;

/// @brief 将 JSON 字符串值转为 YAML 字符串（按需加引号）。
/// @param j 字符串类型的 JSON 值。
/// @return 无需引号时原样输出；否则为双引号转义形态。
[[nodiscard]] inline auto emit_string(const Json &j) -> std::string {
    auto s = j.as_or<std::string>("");
    return yaml_needs_quoting(s) ? yaml_quote_string(s) : std::move(s);
}

/// @brief 将 JSON 浮点值转为 YAML 浮点字符串。
/// @param v 浮点值；NaN/±Inf 映射为 .nan/±.inf。
/// @return 保证含小数点或指数标记的 YAML 浮点字面量。
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
/// @param j 待转换的 JSON 值；非字符串标量按各自字面量形态输出。
/// @return null/true/false/整数/浮点/字符串的 YAML 表示；未知类型回退为 "null"。
[[nodiscard]] inline auto emit_scalar(const Json &j) -> std::string {
    if (j.is_null()) {
        return "null";
    }
    if (j.is_bool()) {
        return j.as_or<bool>(false) ? "true" : "false";
    }
    if (j.is_int()) {
        return std::to_string(j.as_or<std::int64_t>(0));
    }
    if (j.is_uint()) {
        return std::to_string(j.as_or<std::uint64_t>(0));
    }
    if (j.is_double()) {
        return emit_float(j.as_or<double>(0.0));
    }
    if (j.is_string()) {
        return emit_string(j);
    }
    return "null";  // fallback
}

/// @brief 把子块首行的自身缩进让给调用方的 `- ` 前缀。
/// `- ` 恰占两列，与「下一级缩进 = 2 空格」同宽，故只需削掉首行的前导空格，
/// 子块的其余行就已对齐在同一列上——这正是 YAML 列表项内映射的合法排版。
/// @param block 待削减的子块文本。
/// @param cols 让出的前导列数（调用方前缀的宽度）。
/// @return 削去首行 cols 个字符后的文本；block 短于 cols 时原样返回。
[[nodiscard]] inline auto drop_first_indent(const std::string &block, std::size_t cols) -> std::string {
    return block.size() >= cols ? block.substr(cols) : block;
}

[[nodiscard]] inline auto emit_array(const Json &j, int indent, const std::string &pad) -> std::string {
    std::ostringstream os;
    const std::string child_pad(static_cast<std::size_t>(indent + 1) * 2, ' ');
    for (std::size_t i = 0; i < j.size(); ++i) {
        if (i > 0) {
            os << '\n';
        }
        const Json *item = j.at(i);
        os << pad << "- ";
        if (item->is_object()) {
            os << (item->empty() ? "{}"
                                 : drop_first_indent(emit_object(*item, indent + 1, child_pad), child_pad.size()));
        } else if (item->is_array()) {
            os << (item->empty() ? "[]"
                                 : drop_first_indent(emit_array(*item, indent + 1, child_pad), child_pad.size()));
        } else {
            os << emit_scalar(*item);
        }
    }
    return os.str();
}

/// @brief 将 JSON 对象的一个键值对转为 YAML 行块。
/// 容器值**另起一块**并整体下沉一级缩进——把嵌套映射内联在 `key:` 同一行会抹掉层级，产出的不是合法
/// YAML（多行块的首行被 `- ` 或调用方前缀接管，故此处只负责键行与其后块）。
/// @param val 该键对应的值（标量内联在键行；容器另起下沉块；空容器内联 {} / []）。
/// @param key 键名（按需加引号转义）。
/// @param indent 本层级缩进级（容器值按 indent + 1 下沉）。
/// @param pad 键行的行首前缀。
/// @return 键行及其后子块组成的 YAML 片段（无末尾换行）。
[[nodiscard]] inline auto emit_object_value(const Json &val, const std::string &key, int indent, const std::string &pad)
    -> std::string {
    std::ostringstream os;
    const std::string yaml_key = yaml_needs_quoting(key) ? yaml_quote_string(key) : key;
    const std::string child_pad(static_cast<std::size_t>(indent + 1) * 2, ' ');
    os << pad << yaml_key << ":";
    if (val.is_object()) {
        if (val.empty()) {
            os << " {}";
        } else {
            os << '\n' << emit_object(val, indent + 1, child_pad);
        }
    } else if (val.is_array()) {
        if (val.empty()) {
            os << " []";
        } else {
            os << '\n' << emit_array(val, indent + 1, child_pad);
        }
    } else {
        os << ' ' << emit_scalar(val);
    }
    return os.str();
}

[[nodiscard]] inline auto emit_object(const Json &j, int indent, const std::string &pad) -> std::string {
    std::ostringstream os;
    bool first = true;
    for (const auto &e : j.entries()) {
        if (!first) {
            os << '\n';
        }
        first = false;
        os << emit_object_value(e.value, std::string(e.key), indent, pad);
    }
    return os.str();
}

[[nodiscard]] inline auto yaml_emit(const Json &j, int indent) -> std::string {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');

    if (j.is_object()) {
        return j.empty() ? std::string{"{}"} : emit_object(j, indent, pad);
    }
    if (j.is_array()) {
        return j.empty() ? std::string{"[]"} : emit_array(j, indent, pad);
    }
    return emit_scalar(j);
}

}  // namespace detail

/// @brief 将 JSON 值转换为 YAML 格式字符串（2 空格缩进）。
/// @param j 待转换的 JSON 值。
/// @param indent 起始缩进层级（默认 0，即顶格）。
/// @return detail::yaml_emit 产出的 YAML 文本。
[[nodiscard]] inline auto to_yaml(const Json &j, int indent = 0) -> std::string { return detail::yaml_emit(j, indent); }

}  // namespace serialization
}  // namespace aurora
