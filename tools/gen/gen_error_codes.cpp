// ============================================================================
// gen_error_codes — Aurora error-code generator
// ----------------------------------------------------------------------------
// Reads codespec/errors.toml (single declaration source) and produces:
//   1) include/aurora/core/error_codes.gen.h  (enum + ErrorMeta table + lookup functions)
//   2) codespec/ERROR_CATALOG.md              (human-readable catalog, auto-generated)
//   3) the "error_codes" section of aurora_api.json (machine-readable contract)
//
// Usage: gen_error_codes <errors.toml> <error_codes.gen.h> <ERROR_CATALOG.md> <aurora_api.json>
// All four paths are required: output paths that default to the source tree would bypass the
// generate_error_codes target's clang-format step and leave unformatted content in version control.
//
// Note: this tool is a build-time generator; it does not link the Aurora widget/backend layers and does not
//       depend on Aurora UI headers — it only uses the standard library and the aurora core/json headers.
// ============================================================================
#include <aurora/widget/props_io.h>

#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "api_json_merge.h"
#include "toml_lines.h"

namespace {

using aurora::Json;  // aurora 自研 JSON 值类型（props_io.h 别名），类型名保持 Json 不变

// This tool is a build-time generator and does not link Aurora, so aurora's Logger/AURORA_LOG_* is unavailable.
// To honor the project convention of "never touching the standard streams directly", all diagnostic output is
// funneled through err() below: single definition, uniform prefix, ready to be bridged to a logging framework later.
auto err(const std::string &msg) -> void { std::cerr << "[gen_error_codes] " << msg << "\n"; }

struct ErrorEntry {
    std::string enum_name;
    std::string slug;
    std::string domain;
    std::string kind;
    std::string severity;
    bool retryable = false;
    std::string message;
    std::string hint;
};

// The order of domain / kind / severity is the underlying order of the generated enums; append new values at the end.
// constexpr std::array is used instead of C arrays: avoids C-style array warnings and satisfies constant global
// naming and bounds checking. The AURORA_ prefix satisfies the global-constant naming rule (GlobalConstantPrefix).
//
// domain answers "which subsystem do I go look in", kind answers "what layer broke and what action follows". They are
// orthogonal: the same domain spreads over several kinds and vice versa, so neither array may be derived from the
// other. The declaration order of AURORA_DOMAINS is the order the generated enum (and therefore the toml entry order)
// follows; the declaration order of AURORA_KINDS doubles as the classification priority: the first kind whose
// description matches an error owns it, `internal` last as the fallback.
constexpr std::array<const char *, 29> AURORA_DOMAINS = {
    "general",
    "layout",
    "widget",
    "render",
    "navigation",
    "validation",
    "generation",
    "runtime",
    "io",
    "json",
    "compression",
    "storage",
    "i18n",
    "prefs",
    "icon",
    "platform",
    "window",
    "dialog",
    "clipboard",
    "drag",
    "menu",
    "notification",
    "recent-documents",
    "webview",
    "audio",
    "cli",
    "net",
    "http",
    "ws",
};
constexpr std::array<const char *, 12> AURORA_KINDS = {
    "invalid",   "not-found", "unsupported", "unavailable", "denied", "conflict",
    "exhausted", "timeout",   "cancelled",   "corrupt",     "io",     "internal",
};
constexpr std::array<const char *, 4> AURORA_SEVERITIES = {"info", "warning", "error", "fatal"};

// Doc text emitted next to each enumerator (codespec/CODING_STANDARDS.md §13.5.2 requires one description per
// enumerator). Kept parallel to the value arrays above: same order, same length, indexed by the same position.
constexpr std::array<const char *, 29> AURORA_DOMAIN_DOC = {
    "cross-module fallback; no specific subsystem owns the error",
    "layout measurement and constraint errors",
    "widget contract and property errors",
    "painter, font and bitmap rendering errors",
    "page stack and routing errors",
    "input and configuration validation errors",
    "UI generation (generate_ui) errors",
    "runtime environment and lifecycle state errors",
    "file and byte-stream read/write errors (generic domain)",
    "JSON parsing and serialization errors",
    "compression codec encode/decode errors",
    "key-value storage backend and record errors",
    "localization message catalog errors",
    "preference persistence errors",
    "icon resource errors (semantic name, glyph, SVG)",
    "platform backend and OS integration errors (generic domain)",
    "window lifecycle errors (surface and device)",
    "platform dialog creation errors",
    "system clipboard errors",
    "system drag-and-drop errors",
    "native menu bar errors",
    "system notification errors",
    "system recent-documents projection errors",
    "native web view interaction and snapshot errors",
    "audio graph and device errors",
    "command line argument parsing errors",
    "network socket layer errors",
    "HTTP client errors",
    "WebSocket errors",
};
constexpr std::array<const char *, 12> AURORA_KIND_DOC = {
    "argument or declaration is syntactically invalid or out of range",
    "name is well formed but the registry, catalog or route table has no such entry",
    "this build, platform or implementation lacks the capability or format",
    "a dependency exists but is not usable right now (not ready, closed, removed, degraded fallback)",
    "permission, policy or credential was refused",
    "two individually valid declarations or states are mutually exclusive (cycle, duplicate name, replay)",
    "a limit, capacity, depth, queue or chain length was reached",
    "the operation did not complete within its deadline",
    "the caller or the owner cancelled the operation",
    "input or peer bytes do not match expected shape (parse, magic, version, truncation, protocol)",
    "a low-level primitive action failed (write, send, recv, connect, listen, bind, system call)",
    "an invariant broke, an unexpected exception escaped, or no other kind fits",
};
constexpr std::array<const char *, 4> AURORA_SEVERITY_DOC = {
    "reserved level; no codespec/errors.toml entry declares it",
    "degraded condition, logged as LogLevel::Warn",
    "failed operation, logged as LogLevel::Error",
    "reserved level; no codespec/errors.toml entry declares it",
};

// C++ enumerator names for the two kebab-case value lists, spelled out instead of derived by rule.
// The mapping is not mechanical: `io` becomes `Io` as a domain but `IO` as a kind (the all-caps acronym was chosen
// deliberately), `i18n` becomes `I18n`, `recent-documents` becomes `RecentDocuments`, `not-found` becomes `NotFound`.
// A derivation function would bury the `IO` special case as implicit knowledge inside code; parallel arrays keep it
// grep-able and reviewable, and mirror the DOC arrays' existing convention. capitalize() below therefore stays
// reserved for ErrorSeverity, whose names are single-segment words with no per-case exceptions.
constexpr std::array<const char *, 29> AURORA_DOMAIN_NAMES = {
    "General",
    "Layout",
    "Widget",
    "Render",
    "Navigation",
    "Validation",
    "Generation",
    "Runtime",
    "Io",
    "Json",
    "Compression",
    "Storage",
    "I18n",
    "Prefs",
    "Icon",
    "Platform",
    "Window",
    "Dialog",
    "Clipboard",
    "Drag",
    "Menu",
    "Notification",
    "RecentDocuments",
    "Webview",
    "Audio",
    "Cli",
    "Net",
    "Http",
    "Ws",
};
constexpr std::array<const char *, 12> AURORA_KIND_NAMES = {
    "Invalid",   "NotFound", "Unsupported", "Unavailable", "Denied", "Conflict",
    "Exhausted", "Timeout",  "Cancelled",   "Corrupt",     "IO",     "Internal",
};

// trim / parse_kv / unquote are provided by toml_lines.h (shared with gen_debug_api).

auto cpp_escape(const std::string &s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// capitalize the first letter (ErrorSeverity names are single-segment words, so no per-case exceptions apply)
auto capitalize(const std::string &s) -> std::string {
    if (s.empty()) {
        return s;
    }
    std::string r = s;
    r.at(0) = static_cast<char>(std::toupper(static_cast<unsigned char>(r.at(0))));
    return r;
}

// Locate a kebab value inside a parallel (values, names) pair and return its C++ enumerator name.
// Returns an empty string when the value is absent, which the callers treat as a hard error: parse_toml rejects
// such entries before any artifact is written, so the emitter paths are only reached with whitelisted values.
template <std::size_t N>
auto cpp_name_for(std::string_view value, const std::array<const char *, N> &values,
                  const std::array<const char *, N> &names) -> std::string {
    for (size_t i = 0; i < N; ++i) {
        if (value == values.at(i)) {
            return std::string(names.at(i));
        }
    }
    return {};
}

// Write a parsed key/value into the corresponding ErrorEntry field (extracted to lower parse_toml cognitive
// complexity).
auto apply_field(ErrorEntry &e, const std::string &k, const std::string &v) -> void {
    if (k == "enum") {
        e.enum_name = unquote(v);
    } else if (k == "slug") {
        e.slug = unquote(v);
    } else if (k == "domain") {
        e.domain = unquote(v);
    } else if (k == "kind") {
        e.kind = unquote(v);
    } else if (k == "severity") {
        e.severity = unquote(v);
    } else if (k == "message") {
        e.message = unquote(v);
    } else if (k == "hint") {
        e.hint = unquote(v);
    } else if (k == "retryable") {
        e.retryable = v == "true";
    }
}

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
        if (!t.empty() && t.at(0) == '[') {  // top-level table, ignored
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
        // domain / kind are hard requirements over a closed value set: an empty or off-list value aborts the run
        // rather than defaulting. The retired `category` column silently fell back to "general" when omitted, which is
        // exactly how entries accumulated labels nobody chose; a typo such as `timout` would likewise mint a bogus kind
        // that no consumer can route on. Fail loudly, write no artifact.
        if (e.domain.empty() || e.kind.empty()) {
            err("error entry missing domain/kind: " + e.slug);
            return false;
        }
        if (cpp_name_for(e.domain, AURORA_DOMAINS, AURORA_DOMAIN_NAMES).empty()) {
            err("error domain is outside the declared list: " + e.domain + " (entry " + e.slug + ")");
            return false;
        }
        if (cpp_name_for(e.kind, AURORA_KINDS, AURORA_KIND_NAMES).empty()) {
            err("error kind is outside the declared list: " + e.kind + " (entry " + e.slug + ")");
            return false;
        }
        if (e.severity.empty()) {
            e.severity = "error";
        }
    }
    // Zero-value invariant: `ErrorCode{}` is the default sentinel in Error::code_enum and Diagnostic::code_enum, so row
    // 0 must stay GeneralUnknown. Once entries are ordered by domain that position becomes a property of the data
    // rather than of history, so it needs a mechanical guard instead of author discipline.
    if (out.empty() || out.front().enum_name != "GeneralUnknown" || out.front().slug != "general-unknown") {
        err("first entry must be GeneralUnknown / general-unknown (ErrorCode{} is the default sentinel)");
        return false;
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
         "/// @brief C++ mirror of codespec/errors.toml: error enums, the ErrorMeta table and lookup helpers.\n"
         "/// @file\n"
         "#pragma once\n"
         "#include <array>\n"
         "#include <cstdint>\n"
         "#include <string>\n"
         "#include <string_view>\n"
         "#include <unordered_map>\n\n"
         "namespace aurora {\n\n"
         "/// @brief key-value table of error args (message template {key} rendered from it)\n"
         "using ErrorParams = std::unordered_map<std::string, std::string>;\n\n";

    // ErrorDomain / ErrorKind: two orthogonal axes - which subsystem owns it, and what kind of failure it is.
    o << "/// @brief subsystem an error belongs to; answers \"where do I go look\". The declaration order is the\n"
         "///        underlying value order and the codespec/errors.toml entry order.\n"
         "enum class ErrorDomain : std::uint8_t {\n";
    for (size_t i = 0; i < std::size(AURORA_DOMAINS); ++i) {
        o << "    " << AURORA_DOMAIN_NAMES.at(i) << " = " << i << ",   ///< " << AURORA_DOMAIN_DOC.at(i) << "\n";
    }
    o << "};\n\n";

    o << "/// @brief failure nature of an error; answers \"which layer broke and what action follows\". Orthogonal to\n"
         "///        ErrorDomain: one domain spreads over several kinds and vice versa. The declaration order doubles\n"
         "///        as the classification priority -- the first kind whose description matches owns the entry, with\n"
         "///        Internal last as the fallback -- so a borderline call lands on a fixed rule rather than on the\n"
         "///        judgement of whoever added the row.\n"
         "enum class ErrorKind : std::uint8_t {\n";
    for (size_t i = 0; i < std::size(AURORA_KINDS); ++i) {
        o << "    " << AURORA_KIND_NAMES.at(i) << " = " << i << ",   ///< " << AURORA_KIND_DOC.at(i) << "\n";
    }
    o << "};\n\n";

    // ErrorSeverity
    o << "/// @brief severity as declared in codespec/errors.toml (diagnostics logs Error verbatim, everything "
         "else as Warn).\n"
         "enum class ErrorSeverity : std::uint8_t {\n";
    for (size_t i = 0; i < std::size(AURORA_SEVERITIES); ++i) {
        o << "    " << capitalize(AURORA_SEVERITIES.at(i)) << " = " << i << ",    ///< " << AURORA_SEVERITY_DOC.at(i)
          << "\n";
    }
    o << "};\n\n";

    // ErrorCode
    o << "/// @brief one enumerator per entry of codespec/errors.toml; the numeric value is the row index in "
         "AURORA_ERROR_TABLE.\n"
         "enum class ErrorCode : std::uint16_t { // NOLINT(*-enum-size)\n";
    for (size_t i = 0; i < e.size(); ++i) {
        // 尾注长度受 clang-format ColumnLimit(120) 约束：超预算会回折成悬挂行，故只保留 slug。
        const std::string head = "    " + e.at(i).enum_name + " = " + std::to_string(i) + ",  ///< ";
        std::string note = e.at(i).slug + ": " + e.at(i).message;
        if (head.size() + note.size() > 118) {
            note = e.at(i).slug;
        }
        o << head << cpp_escape(note) << "\n";
    }
    o << "};\n\n";

    // ErrorMeta
    o << "/// @brief static metadata row of one error code (everything the catalog knows about it).\n"
         "struct ErrorMeta {\n"
         "    ErrorCode code;                 ///< enumerator of this row\n"
         "    std::string_view ident;         ///< C++ identifier, e.g. \"NavDepthExceeded\" (debug use)\n"
         "    std::string_view slug;          ///< frozen external code, e.g. \"nav-depth-exceeded\"\n"
         "    ErrorDomain domain;             ///< subsystem to look in\n"
         "    ErrorKind kind;                 ///< nature of the failure\n"
         "    ErrorSeverity severity;         ///< declared severity\n"
         "    bool retryable;                 ///< whether re-running the operation may succeed\n"
         "    std::string_view message_tpl;   ///< contains {placeholder}\n"
         "    std::string_view hint;          ///< remediation advice, empty when none is declared\n"
         "};\n\n";

    // AURORA_ERROR_TABLE: the raw table is carried directly by aggregate initialization of
    // std::array<ErrorMeta, N>, where N is derived from the entry count e.size() at generation time,
    // avoiding a hand-written N that could mismatch the entry count (too many / too few initializers);
    // no C-array indexing is used, avoiding cppcoreguidelines-pro-bounds-constant-array-index.
    o << "/// @brief one ErrorMeta row per codespec/errors.toml entry, indexed by ErrorCode's underlying value.\n"
         "inline constexpr std::array<ErrorMeta, "
      << e.size() << "> AURORA_ERROR_TABLE = {\n    {\n";
    for (const auto &[enum_name, slug, domain, kind, severity, retryable, message, hint] : e) {
        o << "        {\n"
          << "            .code = ErrorCode::" << enum_name << ",\n"
          << "            .ident = \"" << cpp_escape(enum_name) << "\",\n"
          << "            .slug = \"" << cpp_escape(slug) << "\",\n"
          << "            .domain = ErrorDomain::" << cpp_name_for(domain, AURORA_DOMAINS, AURORA_DOMAIN_NAMES) << ",\n"
          << "            .kind = ErrorKind::" << cpp_name_for(kind, AURORA_KINDS, AURORA_KIND_NAMES) << ",\n"
          << "            .severity = ErrorSeverity::" << capitalize(severity) << ",\n"
          << "            .retryable = " << (retryable ? "true" : "false") << ",\n"
          << "            .message_tpl = \"" << cpp_escape(message) << "\",\n"
          << "            .hint = \"" << cpp_escape(hint) << "\",\n"
          << "        },\n";
    }
    o << "  },\n};\n\n";

    // helpers
    o << "/// @brief number of declared error codes.\n"
         "/// @return rows in AURORA_ERROR_TABLE.\n"
         "constexpr auto error_count() -> std::size_t { return std::size(AURORA_ERROR_TABLE); }\n\n"
         "/// @brief frozen external code of an error.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return slug as declared in errors.toml.\n"
         "constexpr auto slug(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).slug;\n"
         "}\n"
         "/// @brief subsystem an error code belongs to.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return declared domain.\n"
         "constexpr auto domain(ErrorCode c) -> ErrorDomain {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).domain;\n"
         "}\n"
         "/// @brief failure nature of an error code.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return declared kind.\n"
         "constexpr auto kind(ErrorCode c) -> ErrorKind {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).kind;\n"
         "}\n"
         "/// @brief declared severity of an error code.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return declared severity.\n"
         "constexpr auto severity(ErrorCode c) -> ErrorSeverity {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).severity;\n"
         "}\n"
         "/// @brief whether re-running the failed operation may succeed.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return declared retryable flag.\n"
         "constexpr auto retryable(ErrorCode c) -> bool {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).retryable;\n"
         "}\n"
         "/// @brief remediation advice of an error code.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return hint text, empty when none is declared.\n"
         "constexpr auto hint_of(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).hint;\n"
         "}\n"
         "/// @brief debug-only textual identifier of an error code.\n"
         "/// @param[in] c error code to look up.\n"
         "/// @return C++ identifier; it may change on rename, so it is not an external contract.\n"
         "constexpr auto to_string(ErrorCode c) -> std::string_view {\n"
         "    return AURORA_ERROR_TABLE.at(static_cast<std::size_t>(c)).ident;\n"
         "}\n\n";

    // to_string for domain/kind/severity (kebab)
    o << "/// @brief kebab-case name of an error domain.\n"
         "/// @param[in] d domain to render.\n"
         "/// @return declared domain name, \"unknown\" for a value outside the enum.\n"
         "constexpr auto to_string(ErrorDomain d) -> std::string_view {\n"
         "    switch (d) {\n";
    for (size_t i = 0; i < std::size(AURORA_DOMAINS); ++i) {
        o << "        case ErrorDomain::" << AURORA_DOMAIN_NAMES.at(i) << ": return \"" << AURORA_DOMAINS.at(i)
          << "\";\n";
    }
    o << "    }\n    return \"unknown\";\n}\n\n";

    // `io` is both a domain and a kind; the two to_string overloads keep them apart by static type, and the JSON keys
    // (domain / kind) keep them apart on the wire. domain="io" reads as the generic file/byte-stream domain,
    // kind="io" as a failed low-level primitive -- the identical spelling is not a claim that one contains the other.
    o << "/// @brief kebab-case name of an error kind.\n"
         "/// @param[in] k kind to render.\n"
         "/// @return declared kind name, \"unknown\" for a value outside the enum.\n"
         "constexpr auto to_string(ErrorKind k) -> std::string_view {\n"
         "    switch (k) {\n";
    for (size_t i = 0; i < std::size(AURORA_KINDS); ++i) {
        o << "        case ErrorKind::" << AURORA_KIND_NAMES.at(i) << ": return \"" << AURORA_KINDS.at(i) << "\";\n";
    }
    o << "    }\n    return \"unknown\";\n}\n\n";

    o << "/// @brief kebab-case name of a severity level.\n"
         "/// @param[in] s severity to render.\n"
         "/// @return declared severity name, \"error\" for a value outside the enum.\n"
         "constexpr auto to_string(ErrorSeverity s) -> std::string_view {\n"
         "    switch (s) {\n";
    for (const auto *const i : AURORA_SEVERITIES) {
        o << "        case ErrorSeverity::" << capitalize(i) << ": return \"" << i << "\";\n";
    }
    o << "    }\n    return \"error\";\n}\n\n";

    // format_message
    o << "/// @brief render a message template, substituting {key} occurrences from params.\n"
         "/// @param[in] tpl message template as declared in errors.toml.\n"
         "/// @param[in] params key-value table supplying the substitutions.\n"
         "/// @return rendered text; a {key} missing from params is kept verbatim.\n"
         "inline auto format_message(std::string_view tpl, const ErrorParams& params) -> std::string {\n"
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
         "identifier and may be renamed freely.\n"
         "> `domain` says which subsystem owns the code, `kind` says what broke there; the two columns are orthogonal, "
         "so read them as a pair (`storage` + `io` means a failed low-level primitive inside the storage domain).\n\n"
         "| # | enum | slug | domain | kind | severity | retryable | message | hint |\n"
         "|---|------|------|--------|------|----------|-----------|---------|------|\n";
    for (size_t i = 0; i < e.size(); ++i) {
        const auto &x = e.at(i);
        o << "| " << i << " | `" << x.enum_name << "` | `" << x.slug << "` | " << x.domain << " | " << x.kind << " | "
          << x.severity << " | " << (x.retryable ? "true" : "false") << " | " << x.message << " | " << x.hint << " |\n";
    }
    o << "\n<!-- count: total " << e.size() << " error codes -->\n";
    return o.str();
}

auto gen_api_json(const std::vector<ErrorEntry> &e) -> Json {
    // 不输出 index：它恒等于数组下标（91/91），消费者按位置派生即可。
    // slug 保留：它不是机械可推导的（RuntimeAsyncTimeout→async-timeout、GenerateUiEmpty→
    // generation-ui-empty 两处为语义改写），且是冻结外部契约（见 gen_catalog 头注）。
    Json arr = Json::array();
    for (const auto &x : e) {
        Json o = Json::object();
        o.set("enum", x.enum_name);
        o.set("slug", x.slug);
        o.set("domain", x.domain);
        o.set("kind", x.kind);
        o.set("severity", x.severity);
        o.set("retryable", Json{x.retryable});
        o.set("message", x.message);
        o.set("hint", x.hint);
        arr.push_back(std::move(o));
    }
    return arr;
}

// interchangeable
void write_file(const std::string &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

// This tool intentionally uses exceptions (e.g. try/catch around json parsing in main, std::string allocation,
// ofstream failures), so main should not be forced to noexcept; hence exception-escape is suppressed.
auto main(int argc, char **argv) -> int {  // NOLINT(bugprone-exception-escape)
    // 四个路径一律要求显式给出。曾允许省略并默认写 `include/aurora/core/error_codes.gen.h` 等
    // 源码树文件：那条路径绕过了 generate_error_codes 的 clang-format 步骤（生成器自己写出的是
    // 单行超长形态），把未排版产物直接留在版控文件里，之后任何一次外部格式化都会把它折成非仓库
    // 风格并把 check_doc_comments / format-check 弄红。细则见 cmake/AuroraTools.cmake 的生成注释。
    if (argc < 5) {
        err("usage: gen_error_codes <errors.toml> <error_codes.gen.h> <ERROR_CATALOG.md> <aurora_api.json>");
        err("refusing to write generator outputs without explicit paths");
        return 1;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
    const std::string toml = argv[1];
    const std::string gen_h = argv[2];
    const std::string catalog = argv[3];
    const std::string api = argv[4];
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)

    std::vector<ErrorEntry> entries;
    if (!parse_toml(toml, entries) || entries.empty()) {
        err("parse failed or empty");
        return 1;
    }

    write_file(gen_h, gen_header(entries));
    write_file(catalog, gen_catalog(entries));

    // Update the "error_codes" section of aurora_api.json (preserving other keys). The merge-only semantics are
    // provided by api_json_merge.h: truncation protection, abort on corruption, never write an empty object over
    // other sections.
    if (!aurora::tools::merge_api_json_section(api, "error_codes", gen_api_json(entries), err)) {
        return 1;
    }

    err("generation complete:" + std::to_string(entries.size()) + " error codes (" + gen_h + ", " + catalog + ", " +
        api + ")");
    return 0;
}
