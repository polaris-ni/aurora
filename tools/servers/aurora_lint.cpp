// tools/servers/aurora_lint.cpp
//
// aurora_lint — Aurora UI-tree structural checker.
//
// Performs static structural validation on a serialized UI tree (the JSON output
// of au::serialization::to_json), producing structured, machine-readable diagnostics
// (instead of crashing) when code is partially missing or input is invalid,
// consistent with the runtime Diagnostics philosophy of "degrade rather than abort".
//
// Usage:
//   aurora_lint <tree.json> [--format text|json]
//       - validate the UI tree in the file; text is human-readable, json is a diagnostic array.
//       - exit code: 1 if any error-level issue exists, otherwise 0 (warn/info do not fail).
//   aurora_lint --explain <code>
//       - print a human-readable explanation of a diagnostic code (reuses au::Diagnostics::explain_diagnostic).
//   aurora_lint --list
//       - list all currently registered component types.
//   aurora_lint --help
//
// Note: this tool is a standalone CLI binary (not exposed via MCP/CLI); it is unrelated
// to "build/test/lint exposed via MCP", the latter of which has been explicitly scoped out.

#include <aurora/aurora.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "au_lint_core.h"
#include "json_file.h"

namespace {

// File-reading primitives are provided by tools/include/json_file.h (au::tools::read_text_file).
// Core lint logic lives in tools/include/au_lint_core.h (lint_ui_tree, also reused by tests/test_au_lint.cpp).

void print_text(const std::vector<au::tools::LintFinding> &findings) {
    if (findings.empty()) {
        AURORA_LOG_RAW("aulint", "ok: no structural issues found.\n");
        return;
    }
    for (const auto &f : findings) {
        if (!f.path.empty()) {
            AURORA_LOG_RAW("aulint", "[", au::to_string(f.severity), "] ", f.code, ": ", f.message, "  @", f.path,
                           "\n");
        } else {
            AURORA_LOG_RAW("aulint", "[", au::to_string(f.severity), "] ", f.code, ": ", f.message, "\n");
        }
    }
    int errs = 0;
    int warns = 0;
    int infos = 0;
    for (const auto &f : findings) {
        if (f.severity == au::ErrorSeverity::Error) {
            ++errs;
        } else if (f.severity == au::ErrorSeverity::Warning) {
            ++warns;
        } else {
            ++infos;
        }
    }
    AURORA_LOG_RAW("aulint", "---\n", errs, " error(s), ", warns, " warning(s), ", infos, " info\n");
}

void print_json(const std::vector<au::tools::LintFinding> &findings) {
    au::storage::Json arr = au::storage::Json::array();
    for (const auto &f : findings) {
        au::storage::Json o = au::storage::Json::object();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["severity"] = au::to_string(f.severity);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["code"] = f.code;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["message"] = f.message;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["path"] = f.path;
        arr.push_back(std::move(o));
    }
    AURORA_LOG_RAW("aulint", arr.dump(2), "\n");
}

void print_help() {
    AURORA_LOG_RAW("aulint",
                   "aurora_lint -- Aurora UI tree structural checker\n"
                   "Usage:\n"
                   "  aurora_lint <tree.json> [--format text|json]   validate serialized UI tree\n"
                   "  aurora_lint --explain <code>                   explain a diagnostic code\n"
                   "  aurora_lint --list                             list registered component types\n"
                   "  aurora_lint --help                             show this help\n"
                   "Exit code: 1 if any error-level issue exists, otherwise 0.\n");
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main(int argc, char **argv) -> int {
    std::vector<std::string> args(argv + 1, argv + argc);  // NOLINT(*-pro-bounds-pointer-arithmetic)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_help();
        return 0;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    if (args[0] == "--list") {
        for (const auto &t : au::list_all_components()) {
            AURORA_LOG_RAW("aulint", t, "\n");
        }
        return 0;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    if (args[0] == "--explain") {
        if (args.size() < 2) {
            AURORA_LOG_ERROR("aulint", "error: --explain requires a diagnostic code argument");
            return 2;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_LOG_RAW("aulint", au::Diagnostics::explain_diagnostic(args[1]), "\n");
        return 0;
    }

    // default mode: validate a file
    std::string path;
    std::string format = "text";
    for (std::size_t i = 0; i < args.size(); ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (args[i] == "--format" && i + 1 < args.size()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            format = args[++i];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        } else if (args[i].starts_with("--")) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            AURORA_LOG_ERROR("aulint", "error: unknown option ", args[i]);
            return 2;
        } else {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            path = args[i];
        }
    }
    if (path.empty()) {
        AURORA_LOG_ERROR("aulint", "error: no UI tree file specified for validation");
        return 2;
    }

    const std::string src = au::tools::read_text_file(path);
    if (src.empty()) {
        AURORA_LOG_ERROR("aulint", "error: cannot read file: ", path);
        return 2;
    }

    au::storage::Json tree;
    try {
        tree = au::storage::Json::parse(src);
    } catch (const std::exception &e) {
        AURORA_LOG_ERROR("aulint", "error: JSON parse failed: ", e.what());
        return 2;
    }

    const std::vector<au::tools::LintFinding> findings = au::tools::lint_ui_tree(tree);

    if (format == "json") {
        print_json(findings);
    } else {
        print_text(findings);
    }

    const bool has_error = std::ranges::any_of(
        findings, [](const au::tools::LintFinding &f) -> bool { return f.severity == au::ErrorSeverity::Error; });
    return has_error ? 1 : 0;
}