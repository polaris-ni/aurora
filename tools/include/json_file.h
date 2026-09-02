// ============================================================================
// json_file.h — file-reading primitives (JSON / raw text)
// ----------------------------------------------------------------------------
// Zero aurora dependencies (standard library + nlohmann/json only). Reused by
// aurora_cli / aurora_lint to avoid two duplicate implementations of
// "ifstream + rdbuf reading into string/Json".
//
// Note: on read failure an empty value is returned (Json{} / ""), and nothing is logged here —
// the caller is responsible for diagnostics, consistent with the behavior before extraction
// (the cli checks is_discarded itself after read_json_file returns and then reports the error).
// ============================================================================
#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace aurora::tools {

// Read and parse JSON from a file; on failure (cannot open / parse error) return a discarded Json.
inline auto read_json_file(const std::string &path) -> nlohmann::json {
    std::ifstream f(path);
    if (!f.is_open()) {
        return nlohmann::json{};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str(), nullptr, false);
}

// Read raw text from a file; on failure return an empty string.
inline auto read_text_file(const std::string &path) -> std::string {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace aurora::tools
