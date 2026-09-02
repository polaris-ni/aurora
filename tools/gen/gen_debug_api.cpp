// ============================================================================
// gen_debug_api — Aurora 调试能力 API 生成器
// ----------------------------------------------------------------------------
// 读取 codespec/debug_api.toml（单一声明源），产出 aurora_api.json 的 "debug" 段。
//
// 行为：merge-only —— 读取现有 aurora_api.json 的全部其它段（widgets / enums /
//       error_codes / layout_rules / state_patterns / ...），仅写入 / 覆盖 "debug"
//       段后写回原文件。这样无论本工具与 gen_api_tools / gen_error_codes 以何种顺序运行，
//       都不会相互清掉对方的段。
//
// 截断防护（对应 gen_error_codes 的历史陷阱）：
//   - 现有 aurora_api.json 打开失败（文件缺失） → 视为首次生成，从空对象起；
//   - 现有文件解析失败（疑似损坏 / 截断） → 报错退出，**绝不写空对象**，
//     以免覆盖掉好的其它段（widgets/enums/error_codes 等永久丢失）。
//
// 注意：本工具为构建期生成器，不链接 Aurora 库，不依赖 Aurora 头文件，
//       仅使用标准库与 third_party/nlohmann/json.hpp。
// ============================================================================
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api_json_merge.h"
#include "toml_lines.h"

// 版本单一事实来源（AURORA_VERSION_STRING）。本生成器不链接 aurora，仅借用此纯宏头
// （不引入任何链接符号）；未注入版本宏时回退到 version.h 内置默认值，与库当前版本一致。
#include "aurora/core/version.h"

namespace {

// 本工具为构建期生成器，不链接 Aurora 库，故无法使用 aurora 的 Logger。
// 所有诊断输出统一经由 err() 收口（一处定义、统一前缀）。
auto err(const std::string &msg) -> void { std::cerr << "[gen_debug_api] " << msg << "\n"; }

struct DebugEntry {
    std::string name;
    std::string since;
    std::string gated;
    std::string signature;
    std::string summary;
    std::string header;
};

// trim / parse_kv / unquote 由 toml_lines.h 提供（与 gen_error_codes 共用）。

// NOLINTNEXTLINE(*-function-cognitive-complexity)
auto parse_toml(const std::string &path, std::vector<DebugEntry> &out) -> bool {
    std::ifstream in(path);
    if (!in) {
        err("无法打开输入文件: " + path);
        return false;
    }
    std::string line;
    DebugEntry *cur = nullptr;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        if (t == "[[function]]") {
            out.emplace_back();
            cur = &out.back();
            continue;
        }
        if (!t.empty() && t[0] == '[') { // 顶层表，忽略
            cur = nullptr;
            continue;
        }
        std::string k;
        std::string v;
        if ((cur != nullptr) && parse_kv(t, k, v)) {
            if (k == "name") {
                cur->name = unquote(v);
            } else if (k == "since") {
                cur->since = unquote(v);
            } else if (k == "gated") {
                cur->gated = unquote(v);
            } else if (k == "signature") {
                cur->signature = unquote(v);
            } else if (k == "summary") {
                cur->summary = unquote(v);
            } else if (k == "header") {
                cur->header = unquote(v);
            }
        }
    }
    for (auto &e : out) {
        if (e.name.empty() || e.signature.empty()) {
            err("调试函数条目缺少 name/signature");
            return false;
        }
        if (e.since.empty()) {
            e.since = AURORA_VERSION_STRING;
        }
        if (e.gated.empty()) {
            e.gated = "AURORA_ENABLE_DEBUG";
        }
    }
    return true;
}

auto gen_debug_json(const std::vector<DebugEntry> &entries) -> nlohmann::json {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &x : entries) {
        nlohmann::json o = nlohmann::json::object();
        o["name"] = x.name;
        o["since"] = x.since;
        o["gated"] = x.gated;
        o["signature"] = x.signature;
        o["summary"] = x.summary;
        if (!x.header.empty()) {
            o["header"] = x.header;
        }
        arr.push_back(o);
    }
    return arr;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) — main 错误经 err() 打印 + return 退出；静态分析对上游 parse_toml / merge_api_json_section 保守
auto main(int argc, char **argv) -> int {
    // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
    const std::string toml = (argc > 1) ? argv[1] : "codespec/debug_api.toml";
    const std::string api = (argc > 2) ? argv[2] : "aurora_api.json";
    // NOLINTEND(*-pro-bounds-pointer-arithmetic)

    std::vector<DebugEntry> entries;
    if (!parse_toml(toml, entries) || entries.empty()) {
        err("解析失败或为空");
        return 1;
    }

    // 读取现有文件（merge-only）；损坏则中止，避免截断其它段。语义由 api_json_merge.h 提供。
    if (!aurora::tools::merge_api_json_section(api, "debug", gen_debug_json(entries), err)) {
        return 1;
    }

    err("生成完成：" + std::to_string(entries.size()) + " 个调试 API (" + api + ")");
    return 0;
}
