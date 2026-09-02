// tools/servers/au-lint.cpp
//
// au-lint —— Aurora UI 树结构化检查器。
//
// 对「序列化后的 UI 树」（aurora::serialization::to_json 的输出，JSON）做静态结构校验，
// 在「部分代码缺失 / 输入非法」时给出结构化、机器可读的诊断（而非崩溃），
// 与运行期 Diagnostics 的「降级而非中止」哲学一致。
//
// 用法：
//   au-lint <tree.json> [--format text|json]
//       - 校验文件中的 UI 树；text 为人类可读，json 为诊断数组。
//       - 退出码：存在 error 级问题为 1，否则 0（warn/info 不失败）。
//   au-lint --explain <code>
//       - 输出某诊断码的人类可读解释（复用 au::Diagnostics::explain_diagnostic）。
//   au-lint --list
//       - 列出当前注册的全部组件类型。
//   au-lint --help
//
// 注：本工具为独立 CLI 二进制（非 MCP/CLI 暴露），与「构建/测试/lint 经 MCP 暴露」
//     无关，后者已被明确移出范围。

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <aurora/aurora.h>

#include "au_lint_core.h"
#include "json_file.h"

using au::Diagnostics;
using au::Json;
using au::list_all_components;
using au::serialization::register_core_widgets;

namespace {

using aurora::tools::LintFinding;
using aurora::tools::lint_ui_tree;

// 读文件原语由 tools/include/json_file.h 提供（aurora::tools::read_text_file）。
// 核心 lint 逻辑见 tools/include/au_lint_core.h（lint_ui_tree，亦供 tests/test_au_lint.cpp 复用）。

void print_text(const std::vector<LintFinding> &findings) {
    if (findings.empty()) {
        AURORA_LOG_RAW("aulint", "ok: 未发现结构问题。\n");
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
        if (f.severity == au::ErrorSeverity::Error)
            ++errs;
        else if (f.severity == au::ErrorSeverity::Warning)
            ++warns;
        else
            ++infos;
    }
    AURORA_LOG_RAW("aulint", "---\n", errs, " error(s), ", warns, " warning(s), ", infos, " info\n");
}

void print_json(const std::vector<LintFinding> &findings) {
    Json arr = Json::array();
    for (const auto &f : findings) {
        Json o = Json::object();
        o["severity"] = au::to_string(f.severity);
        o["code"] = f.code;
        o["message"] = f.message;
        o["path"] = f.path;
        arr.push_back(std::move(o));
    }
    AURORA_LOG_RAW("aulint", arr.dump(2), "\n");
}

void print_help() {
    AURORA_LOG_RAW("aulint", "au-lint —— Aurora UI 树结构化检查器\n"
                             "用法:\n"
                             "  au-lint <tree.json> [--format text|json]   校验序列化 UI 树\n"
                             "  au-lint --explain <code>                   解释诊断码\n"
                             "  au-lint --list                             列出已注册组件类型\n"
                             "  au-lint --help                             显示本帮助\n"
                             "退出码: 存在 error 级问题为 1，否则 0。\n");
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_help();
        return 0;
    }

    if (args[0] == "--list") {
        for (const auto &t : list_all_components()) {
            AURORA_LOG_RAW("aulint", t, "\n");
        }
        return 0;
    }

    if (args[0] == "--explain") {
        if (args.size() < 2) {
            AURORA_LOG_ERROR("aulint", "error: --explain 需要一个诊断码参数");
            return 2;
        }
        AURORA_LOG_RAW("aulint", Diagnostics::explain_diagnostic(args[1]), "\n");
        return 0;
    }

    // 默认模式：校验文件
    std::string path;
    std::string format = "text";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--format" && i + 1 < args.size()) {
            format = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
            AURORA_LOG_ERROR("aulint", "error: 未知选项 ", args[i]);
            return 2;
        } else {
            path = args[i];
        }
    }
    if (path.empty()) {
        AURORA_LOG_ERROR("aulint", "error: 未指定要校验的 UI 树文件");
        return 2;
    }

    const std::string src = aurora::tools::read_text_file(path);
    if (src.empty()) {
        AURORA_LOG_ERROR("aulint", "error: 无法读取文件: ", path);
        return 2;
    }

    Json tree;
    try {
        tree = Json::parse(src);
    } catch (const std::exception &e) {
        AURORA_LOG_ERROR("aulint", "error: JSON 解析失败: ", e.what());
        return 2;
    }

    const std::vector<LintFinding> findings = lint_ui_tree(tree);

    if (format == "json") {
        print_json(findings);
    } else {
        print_text(findings);
    }

    const bool has_error = std::ranges::any_of(findings, [](const LintFinding &f) { return f.severity == au::ErrorSeverity::Error; });
    return has_error ? 1 : 0;
}
