// ============================================================================
// gen_error_codes — Aurora 错误码生成器
// ----------------------------------------------------------------------------
// 读取 codespec/errors.toml（单一声明源），产出：
//   1) include/aurora/core/error_codes.gen.h  (枚举 + ErrorMeta 表 + 查表函数)
//   2) codespec/ERROR_CATALOG.md              (人类可读目录，自动生成)
//   3) aurora_api.json 的 "error_codes" 段     (机器可读契约)
//
// 用法：gen_error_codes <errors.toml> <error_codes.gen.h> <ERROR_CATALOG.md> <aurora_api.json>
// 参数为可选，缺省时退化为与 CMAKE_SOURCE_DIR 相关的相对路径。
//
// 注意：本工具为构建期生成器，不链接 Aurora 库，不依赖 Aurora 头文件，
//       仅使用标准库与 third_party/nlohmann/json.hpp。
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

// 本工具为构建期生成器，不链接 Aurora 库，故无法使用 aurora 的 Logger/AURORA_LOG_*。
// 为遵守「不直接触达标准错误流」的项目约定，所有诊断输出统一经由下方 err() 收口：
// 一处定义、统一前缀、可随项目后续桥接到日志框架。
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

// category / severity 顺序即生成枚举的底层序，新增需追加到末尾。
// 用 constexpr std::array 替代 C 数组：避免 C 风格数组告警、满足常量全局命名与边界检查。
// AURORA_ 前缀：满足全局常量命名规则（GlobalConstantPrefix）。
constexpr std::array<const char *, 11> AURORA_CATEGORIES = {
    "general",    "layout",   "widget",  "render",     "io",         "validation",
    "navigation", "platform", "runtime", "generation", "diagnostic",
};
constexpr std::array<const char *, 4> AURORA_SEVERITIES = { "info", "warning", "error", "fatal" };

// trim / parse_kv / unquote 由 toml_lines.h 提供（与 gen_debug_api 共用）。

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

// 首字母大写（category/severity 为单段词）
auto capitalize(const std::string &s) -> std::string {
    if (s.empty()) {
        return s;
    }
    std::string r = s;
    r.at(0) = static_cast<char>(std::toupper(static_cast<unsigned char>(r.at(0))));
    return r;
}

// 将解析出的 key/value 写入 ErrorEntry 对应字段（抽离以降低 parse_toml 认知复杂度）。
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
        if (!t.empty() && t.at(0) == '[') { // 顶层表，忽略
            cur = nullptr;
            continue;
        }
        std::string k;
        std::string v;
        if (cur != nullptr && parse_kv(t, k, v)) {
            apply_field(*cur, k, v);
        }
    }
    // 校验
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
    // 唯一性校验：slug 与 enum 标识符都必须全局唯一。
    // ERROR_CATALOG 自述 slug 为「冻结对外契约」，重复 slug 会让 slug() 查表只命中首条，
    // 导致后续错误被静默映射成错误的码；重复 enum 则生成不可编译的重复枚举值。提前中止。
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
         "// 本文件由 tools/gen/gen_error_codes.cpp 自动生成，请勿手改。\n"
         "// 修改请编辑 codespec/errors.toml 后重新运行生成器。\n"
         "// ============================================================================\n"
         "#pragma once\n"
         "#include <array>\n"
         "#include <cstdint>\n"
         "#include <string>\n"
         "#include <string_view>\n"
         "#include <unordered_map>\n\n"
         "namespace aurora {\n\n"
         "/// @brief 错误参数的键值表（message 模板 {key} 由它渲染）\n"
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
         "    std::string_view ident;         // C++ 标识符，如 \"NavDepthExceeded\"（调试用）\n"
         "    std::string_view slug;          // 冻结对外码，如 \"nav-depth-exceeded\"\n"
         "    ErrorCategory category;\n"
         "    ErrorSeverity severity;\n"
         "    bool auto_fixable;\n"
         "    std::string_view fix_category;\n"
         "    bool retryable;\n"
         "    std::string_view message_tpl;   // 含 {placeholder}\n"
         "    std::string_view hint;\n"
         "};\n\n";

    // AURORA_ERROR_TABLE：直接以 std::array<ErrorMeta, N> 聚合初始化承载原始表，
    // N 由生成时的条目数 e.size() 推导，避免手写 N 与条目数不一致（初始值过多/过少）；
    // 不使用 C 数组下标，规避 cppcoreguidelines-pro-bounds-constant-array-index。
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
         "// 调试用：返回 C++ 标识符（可随改名变），非对外契约\n"
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
    o << "# Aurora 错误码目录（自动生成）\n\n"
         "> 由 `tools/gen/gen_error_codes.cpp` 从 `codespec/errors.toml` 生成，**请勿手改**。\n"
         "> `slug` 为冻结对外契约，跨语言/JSON/日志只认它；`enum` 为 C++ 标识符，可自由改名。\n\n"
         "| # | enum | slug | category | severity | auto_fixable | fix_category | retryable | message | hint |\n"
         "|---|------|------|----------|----------|--------------|--------------|-----------|---------|------|\n";
    for (size_t i = 0; i < e.size(); ++i) {
        const auto &x = e.at(i);
        o << "| " << i << " | `" << x.enum_name << "` | `" << x.slug << "` | " << x.category << " | " << x.severity
          << " | " << (x.auto_fixable ? "true" : "false") << " | " << x.fix_category << " | "
          << (x.retryable ? "true" : "false") << " | " << x.message << " | " << x.hint << " |\n";
    }
    o << "\n<!-- 计数：共 " << e.size() << " 条错误码 -->\n";
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

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): path/content 语义顺序明确，不可互换
void write_file(const std::string &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

// 本工具有意使用异常（如 main 内的 try/catch 解析 json、std::string 内存分配、ofstream 失败），
// main 不应被强制为 noexcept，故抑制 exception-escape。
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

    // 更新 aurora_api.json 的 "error_codes" 段（保留其它键）。merge-only 语义由
    // api_json_merge.h 提供：截断防护、损坏中止，绝不写空对象覆盖其它段。
    if (!aurora::tools::merge_api_json_section(api, "error_codes", gen_api_json(entries), err)) {
        return 1;
    }

    err("generation complete:" + std::to_string(entries.size()) + " error codes (" + genh + ", " + catalog + ", " +
        api + ")");
    return 0;
}
