// ============================================================================
// gen_debug_api — Aurora debug-capability API generator
// ----------------------------------------------------------------------------
// Reads codespec/debug_api.toml (single declaration source) and produces the
// "debug" section of aurora_api.json.
//
// Behavior: merge-only — reads all other sections of the existing aurora_api.json
//       (widgets / enums / error_codes / layout_rules / state_patterns / ...),
//       writes/overwrites only the "debug" section, then writes the file back.
//       This way, no matter what order this tool, gen_api_tools and gen_error_codes
//       run in, they never wipe each other's sections.
//
// Truncation protection (mirroring the historical pitfall of gen_error_codes):
//   - failure to open the existing aurora_api.json (missing file) → treat as first
//     generation, start from an empty object;
//   - failure to parse the existing file (suspected corruption / truncation) →
//     report the error and exit, **never write an empty object**, so as not to
//     overwrite good sections (widgets/enums/error_codes would be lost forever).
//
// Note: this tool is a build-time generator; it does not link the Aurora library and does not
//       depend on Aurora headers — it only uses the standard library and third_party/nlohmann/json.hpp.
// ============================================================================
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "api_json_merge.h"
#include "toml_lines.h"

// Single source of truth for the version (AURORA_VERSION_STRING). This generator does not link aurora;
// it only borrows this macro-only header (introducing no link symbols); when the version macros are not
// injected it falls back to the built-in default in version.h, consistent with the current library version.
#include "aurora/core/version.h"

namespace {

// This tool is a build-time generator and does not link Aurora, so aurora's Logger is unavailable.
// All diagnostic output is funneled through err() (single definition, uniform prefix).
auto err(const std::string &msg) -> void { std::cerr << "[gen_debug_api] " << msg << "\n"; }

struct DebugEntry {
    std::string name;
    std::string since;
    std::string gated;
    std::string signature;
    std::string summary;
    std::string header;
};

// trim / parse_kv / unquote are provided by toml_lines.h (shared with gen_error_codes).

auto parse_toml(const std::string &path, std::vector<DebugEntry> &out) -> bool {
    std::ifstream in(path);
    if (!in) {
        err("cannot open input file: " + path);
        return false;
    }
    std::string line;
    DebugEntry *cur = nullptr;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t.at(0) == '#') {
            continue;
        }
        if (t == "[[function]]") {
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
            err("debug function entry missing name/signature");
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["name"] = x.name;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["since"] = x.since;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["gated"] = x.gated;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["signature"] = x.signature;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        o["summary"] = x.summary;
        if (!x.header.empty()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            o["header"] = x.header;
        }
        arr.push_back(o);
    }
    return arr;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) — main errors are printed via err() then return; static analysis is
// conservative about upstream parse_toml / merge_api_json_section
// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main(int argc, char **argv) -> int {
    // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
    const std::string toml = (argc > 1) ? argv[1] : "codespec/debug_api.toml";
    const std::string api = (argc > 2) ? argv[2] : "aurora_api.json";
    // NOLINTEND(*-pro-bounds-pointer-arithmetic)

    std::vector<DebugEntry> entries;
    if (!parse_toml(toml, entries) || entries.empty()) {
        err("parse failed or empty");
        return 1;
    }

    // Read the existing file (merge-only); abort if corrupt to avoid truncating other sections.
    // Semantics are provided by api_json_merge.h.
    if (!aurora::tools::merge_api_json_section(api, "debug", gen_debug_json(entries), err)) {
        return 1;
    }

    err("generation complete: " + std::to_string(entries.size()) + " debug APIs (" + api + ")");
    return 0;
}