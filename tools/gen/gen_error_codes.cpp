// ============================================================================
// gen_error_codes — Aurora error-code generator
// ----------------------------------------------------------------------------
// Reads codespec/errors.toml (single declaration source) and produces:
//   1) include/aurora/core/error_codes.gen.h  (enum + ErrorMeta table + lookup functions)
//   2) codespec/ERROR_CATALOG.md              (human-readable catalog, auto-generated)
//   3) the "error_codes" section of aurora_api.json (machine-readable contract)
//
// Usage: gen_error_codes <errors.toml> <error_codes.gen.h> <ERROR_CATALOG.md> <aurora_api.json>
// Arguments are optional; when omitted they default to paths relative to CMAKE_SOURCE_DIR.
//
// Note: this tool is a build-time generator; it does not link the Aurora library and does not
//       depend on Aurora headers — it only uses the standard library and third_party/nlohmann/json.hpp.
// ============================================================================
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "api_json_merge.h"
#include "toml_lines.h"

namespace {

// This tool is a build-time generator and does not link Aurora, so aurora's Logger/AURORA_LOG_* is unavailable.
// To honor the project convention of "never touching the standard streams directly", all diagnostic output is
// funneled through err() below: single definition, uniform prefix, ready to be bridged to a logging framework later.
auto err(const std::string &msg) -> void { std::cerr << "[gen_error_codes] " << msg << "\n"; }

struct ErrorEntry {
    std::string enum_name;
    std::string slug;
    std::string category;
    std::string severity;
    bool auto_fixable = false;
    std::string fix_category;
    bool retryable = false;
    std::string message;
    std::string hint;
};

// The order of category / severity is the underlying order of the generated enum; append new entries at the end.
// constexpr std::array is used instead of C arrays: avoids C-style array warnings and satisfies constant global
// naming and bounds checking. The AURORA_ prefix satisfies the global-constant naming rule (GlobalConstantPrefix).
constexpr std::array<const char *, 11> AURORA_CATEGORIES = {
    "general",    "layout",   "widget",  "render",     "io",         "validation",
    "navigation", "platform", "runtime", "generation", "diagnostic",
};
constexpr std::array<const char *, 4> AURORA_SEVERITIES = { "info", "warning", "error", "fatal" };

// trim / parse_kv / unquote are provided by toml_lines.h (shared with gen_debug_api).

auto cpp_escape(const std::string &s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

// capitalize the first letter (category/severity are single-segment words)
auto capitalize(const std::string &s) -> std::string {
    if (s.empty()) {
        return s;
    }
    std::string r = s;
    r.at(0) = static_cast<char>(std::toupper(static_cast<unsigned char>(r.at(0))));
    return r;
}

// Write a parsed key/value into the corresponding ErrorEntry field (extracted to lower parse_toml cognitive
// complexity).
auto apply_field(ErrorEntry &e, const std::string &k, const std::string &v) -> void {
    if (k == "enum") {
        e.enum_name = unquote(v);
    } else if (k == "slug") {
        e.slug = unquote(v);
    } else if (k == "category") {
        e.category = unquote(v);
    } else if (k == "severity") {
        e.severity = unquote(v);
    } else if (k == "fix_category") {
        e.fix_category = unquote(v);
    } else if (k == "message") {
        e.message = unquote(v);
    } else if (k == "hint") {
        e.hint = unquote(v);
    } else if (k == "auto_fixable") {
        e.auto_fixable = v == "true";
    } else if (k == "retryable") {
        e.retryable = v == "true";
    }
}

// NOLINTNEXTLINE(*-function-cognitive-complexity)
auto parse_toml(const std::string &path, std::vector<ErrorEntry> &out) -> bool {
    std::ifstream in(path);
    if (!in) {
        err("cannot open input file: " + path);
        return false;
    }
    std::string line;
    ErrorEntry *cur = nullptr;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t.at(0) == '#') {
            continue;
        }
        if (t == "[[error]]") {
            out.emplace_back();
            cur = &out.back();
            continue;
        }
        if (!t.empty() && t.at(0) == '[') { // top-level table, ignored
            cur = nullptr;
            continue;
        }
        std::string k;
        std::string v;
        if (cur != nullptr && parse_kv(t, k, v)) {
            apply_field(*cur, k, v);
        }
    }
    // validation
    for (auto &e : out) {
        if (e.enum_name.empty() || e.slug.empty()) {
            err("error entry missing enum/slug");
            return false;
        }
        if (e.category.empty()) {
            e.category = "general";
        }
        if (e.severity.empty()) {
            e.severity = "error";
        }
        if (e.fix_category.empty()) {
            e.fix_category = "unknown";
        }
    }
    // Uniqueness check: both slug and enum identifiers must be globally unique.
    // The ERROR_CATALOG describes slug as a "frozen external contract"; duplicate slugs make slug() lookup hit only
    // the first entry, silently mapping later errors to the wrong code; duplicate enums generate uncompilable
    // duplicate enumerator values. Abort early.
    std::unordered_set<std::string> seen_slug;
    std::unordered_set<std::string> seen_enum;
    for (const auto &e : out) {
        if (!seen_slug.insert(e.slug).second) {
            err("duplicate error slug (frozen contract must be unique): " + e.slug);
            return false;
        }
        if (!seen_enum.insert(e.enum_name).second) {
            err("duplicate error enum identifier: " + e.enum_name);
            return false;
        }
    }
    return true;
}

auto gen_header(const std::vector<ErrorEntry> &e) -> std::string {
    std::ostringstream o;
    o << "// ============================================================================\n"
         "// This file is auto-generated by tools/gen/gen_error_codes.cpp; do not edit by hand.\n"
         "// To change it, edit codespec/errors.toml and re-run the generator.\n"
         "// ============================================================================\n"
         "#pragma once\n"
         "#include <array>\n"
         "#include <cstdint>\n"
         "#include <string>\n"
         "#include <string_view>\n"
         "#include <unordered_map>\n\n"
         "namespace aurora {\n\n"
         "/// @brief key-value table of error args (message template {key} rendered from it)\n"
         "using ErrorParams = std::unordered_map<std::string, std::string>;\n\n";

    // ErrorCategory
    o << "enum class ErrorCategory : std::uint8_t {\n";
    for (size_t i = 0; i < std::size(AURORA_CATEGORIES); ++i) {
        o << "    " << capitalize(AURORA_CATEGORIES.at(i)) << " = " << i << ",\n";
    }
    o << "};\n\n";

    // ErrorSeverity
    o << "enum class ErrorSeverity : std::uint8_t {\n";
    for (size_t i = 0; i < std::size(AURORA_SEVERITIES); ++i) {
        o << "    " << capitalize(AURORA_SEVERITIES.at(i)) << " = " << i << ",\n";
    }
    o << "};\n\n";

    // ErrorCode
    o << "enum class ErrorCode : std::uint16_t { // NOLINT(*-enum-size)\n";
    for (size_t i = 0; i < e.size(); ++i) {
        o << "    " << e.at(i).enum_name << " = " << i << ",\n";
    }
    o << "};\n\n";

    // ErrorMeta
    o << "struct ErrorMeta {\n"
         "    ErrorCode code;\n"
         "    std::string_view ident;         // C++ identifier, e.g. \"NavDepthExceeded\" (debug use)\n"
         "    std::string_view slug;          // frozen external code, e.g. \"nav-depth-exceeded\"\n"
         "    ErrorCategory category;\n"
         "    ErrorSeverity severity;\n"
         "    bool auto_fixable;\n"
         "    std::string_view fix_category;\n"
         "    bool retryable;\n"
         "    std::string_view message_tpl;   // contains {placeholder}\n"
         "    std::string_view hint;\n"
         "};\n\n";

    // AURORA_ERROR_TABLE: the raw table is carried directly by aggregate initialization of
    // std::array<ErrorMeta, N>, where N is derived from the entry count e.size() at generation time,
    // avoiding a hand-written N that could mismatch the entry count (too many / too few initializers);
    // no C-array indexing is used, avoiding cppcoreguidelines-pro-bounds-constant-array-index.
    o << "inline constexpr std::array<ErrorMeta, " << e.size() << "> AURORA_ERROR_TABLE = {\n    {\n";
    for (const auto &[enum_name, slug, category, severity, auto_fixable, fix_category, retryable, message, hint] : e) {
        o << "        {\n"
          << "            .code = ErrorCode::" << enum_name << ",\n"
          << "            .ident = \"" << cpp_escape(enum_name) << "\",\n"
          << "            .slug = \"" << cpp_escape(slug) << "\",\n"
          << "            .category = ErrorCategory::" << capitalize(category) << ",\n"
          << "            .severity = ErrorSeverity::" << capitalize(severity) << ",\n"
          << "            .auto_fixable = " << (auto_fixable ? "true" : "false") << ",\n"
          << "            .fix_category = \"" << cpp_escape(fix_category) << "\",\n"
          << "            .retryable = " << (retryable ? "true" : "false") << ",\n"
          << "            .message_tpl = \"" << cpp_escape(message) << "\",\n"
          << "            .hint = \"" << cpp_escape(hint) << "\",\n"
          << "        },\n";
    }
    o << "  },\n};\n\n";

    // helpers
    o << "constexpr auto error_count() -> std::size_t { return std::size(AURORA_ERROR_TABLE); }\n\n"
         "constexpr auto slug(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).slug;\n"
         "}\n"
         "constexpr auto category(ErrorCode c) -> ErrorCategory {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).category;\n"
         "}\n"
         "constexpr auto severity(ErrorCode c) -> ErrorSeverity {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).severity;\n"
         "}\n"
         "constexpr auto is_auto_fixable(ErrorCode c) -> bool {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).auto_fixable;\n"
         "}\n"
         "constexpr auto fix_category_of(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).fix_category;\n"
         "}\n"
         "constexpr auto retryable(ErrorCode c) -> bool {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).retryable;\n"
         "}\n"
         "constexpr auto hint_of(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).hint;\n"
         "}\n"
         "// debug use: returns the C++ identifier (may change on rename), not an external contract\n"
         "constexpr auto to_string(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).ident;\n"
         "}\n\n";

    // to_string for category/severity (kebab)
    o << "constexpr auto to_string(ErrorCategory c) -> std::string_view {\n"
         "    switch (c) {\n";
    for (const auto *const i : AURORA_CATEGORIES) {
        o << "        case ErrorCategory::" << capitalize(i) << ": return \"" << i << "\";\n";
    }
    o << "    }\n    return \"unknown\";\n}\n\n";

    o << "constexpr auto to_string(ErrorSeverity s) -> std::string_view {\n"
         "    switch (s) {\n";
    for (const auto *const i : AURORA_SEVERITIES) {
        o << "        case ErrorSeverity::" << capitalize(i) << ": return \"" << i << "\";\n";
    }
    o << "    }\n    return \"error\";\n}\n\n";

    // format_message
    o << "inline auto format_message(std::string_view tpl, const ErrorParams& params) -> std::string {\n"
         "    std::string out;\n"
         "    out.reserve(tpl.size());\n"
         "    for (std::size_t i = 0; i < tpl.size(); ++i) {\n"
         "        if (tpl[i] == '{') {\n"
         "            const std::size_t j = tpl.find('}', i);\n"
         "            if (j != std::string_view::npos) {\n"
         "                std::string_view key = tpl.substr(i + 1, j - i - 1);\n"
         "                auto it = params.find(std::string(key));\n"
         "                if (it != params.end()) {\n"
         "                    out += it->second;\n"
         "                    i = j;\n"
         "                    continue;\n"
         "                }\n"
         "            }\n"
         "        }\n"
         "        out += tpl[i];\n"
         "    }\n"
         "    return out;\n"
         "}\n\n";

    o << "} // namespace aurora\n";
    return o.str();
}

auto gen_catalog(const std::vector<ErrorEntry> &e) -> std::string {
    std::ostringstream o;
    o << "# Aurora Error Code Catalog (auto-generated)\n\n"
         "> Generated by `tools/gen/gen_error_codes.cpp` from `codespec/errors.toml`; **do not edit by hand**.\n"
         "> `slug` is the frozen external contract — cross-language/JSON/log consumers must use it; `enum` is the C++ "
         "identifier and may be renamed freely.\n\n"
         "| # | enum | slug | category | severity | auto_fixable | fix_category | retryable | message | hint |\n"
         "|---|------|------|----------|----------|--------------|--------------|-----------|---------|------|\n";
    for (size_t i = 0; i < e.size(); ++i) {
        const auto &x = e.at(i);
        o << "| " << i << " | `" << x.enum_name << "` | `" << x.slug << "` | " << x.category << " | " << x.severity
          << " | " << (x.auto_fixable ? "true" : "false") << " | " << x.fix_category << " | "
          << (x.retryable ? "true" : "false") << " | " << x.message << " | " << x.hint << " |\n";
    }
    o << "\n<!-- count: total " << e.size() << " error codes -->\n";
    return o.str();
}

auto gen_api_json(const std::vector<ErrorEntry> &e) -> nlohmann::json {
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < e.size(); ++i) {
        const auto &x = e.at(i);
        arr.push_back({
            { "index", i },
            { "enum", x.enum_name },
            { "slug", x.slug },
            { "category", x.category },
            { "severity", x.severity },
            { "auto_fixable", x.auto_fixable },
            { "fix_category", x.fix_category },
            { "retryable", x.retryable },
            { "message", x.message },
            { "hint", x.hint },
        });
    }
    return arr;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the path/content order is semantically fixed, not
// interchangeable
void write_file(const std::string &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

// This tool intentionally uses exceptions (e.g. try/catch around json parsing in main, std::string allocation,
// ofstream failures), so main should not be forced to noexcept; hence exception-escape is suppressed.
auto main(int argc, char **argv) -> int { // NOLINT(bugprone-exception-escape)
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
    std::string toml = (argc > 1) ? argv[1] : "codespec/errors.toml";
    std::string genh = (argc > 2) ? argv[2] : "include/aurora/core/error_codes.gen.h";
    std::string catalog = (argc > 3) ? argv[3] : "codespec/ERROR_CATALOG.md";
    std::string api = (argc > 4) ? argv[4] : "aurora_api.json";
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)

    std::vector<ErrorEntry> entries;
    if (!parse_toml(toml, entries) || entries.empty()) {
        err("parse failed or empty");
        return 1;
    }

    write_file(genh, gen_header(entries));
    write_file(catalog, gen_catalog(entries));

    // Update the "error_codes" section of aurora_api.json (preserving other keys). The merge-only semantics are
    // provided by api_json_merge.h: truncation protection, abort on corruption, never write an empty object over
    // other sections.
    if (!aurora::tools::merge_api_json_section(api, "error_codes", gen_api_json(entries), err)) {
        return 1;
    }

    err("generation complete:" + std::to_string(entries.size()) + " error codes (" + genh + ", " + catalog + ", " +
        api + ")");
    return 0;
}
