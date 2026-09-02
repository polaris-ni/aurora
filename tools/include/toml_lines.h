// ============================================================================
// toml_lines.h — 极简 TOML「行」解析原语
// ----------------------------------------------------------------------------
// 仅依赖标准库，零 Aurora 依赖。由 gen_error_codes / gen_debug_api 复用，
// 避免「trim / parse_kv / unquote」两份重复实现（二者逐字同义）。
//
// 这些函数以 inline 形式放在全局命名空间，供各生成器以非限定名直接调用，
// 与抽取前的匿名命名空间内定义等价；各生成器是独立可执行文件，不存在跨 TU 的 ODR 问题。
// ============================================================================
#pragma once

#include <cctype>
#include <string>

inline auto trim(const std::string &s) -> std::string {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s.at(a))) != 0) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s.at(b - 1))) != 0) {
        --b;
    }
    return s.substr(a, b - a);
}

// 解析 "key = value" 行，value 可为 "str" / true|false / 整数。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): key/val 语义顺序明确，不可互换
inline auto parse_kv(const std::string &line, std::string &key, std::string &val) -> bool {
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    key = trim(line.substr(0, eq));
    val = trim(line.substr(eq + 1));
    return !key.empty();
}

inline auto unquote(const std::string &v) -> std::string {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}
