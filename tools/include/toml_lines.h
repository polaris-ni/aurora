// ============================================================================
// toml_lines.h — minimal TOML "line" parsing primitives
// ----------------------------------------------------------------------------
// Depends only on the standard library, zero Aurora dependencies. Reused by
// gen_error_codes / gen_debug_api to avoid two duplicate implementations of
// "trim / parse_kv / unquote" (both meaning exactly the same thing).
//
// These functions live in the global namespace as inline definitions so generators can call
// them by unqualified name, equivalent to the previous definitions inside anonymous namespaces;
// each generator is a standalone executable, so there is no cross-TU ODR concern.
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

// Parse a "key = value" line; value may be "str" / true|false / an integer.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the key/val order is semantically fixed, not interchangeable
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
