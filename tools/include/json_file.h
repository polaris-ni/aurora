// ============================================================================
// json_file.h — file-reading primitives (JSON / raw text)
// ----------------------------------------------------------------------------
// Zero third-party dependencies (standard library + aurora core/json only). Reused by
// aurora_cli to avoid two duplicate implementations of
// "ifstream + rdbuf reading into string/Json".
//
// Note: on read failure an empty value is returned (Json{} / ""), and nothing is logged here —
// the caller is responsible for diagnostics, consistent with the behavior before extraction
// (the cli checks the null value itself after read_json_file returns and then reports the error).
// ============================================================================
#pragma once

#include <aurora/widget/props_io.h>

#include <fstream>
#include <sstream>
#include <string>

namespace aurora::tools {

// Read and parse JSON from a file; on failure (cannot open / parse error) return a null Json.
inline auto read_json_file(const std::string &path) -> Json {
    std::ifstream f(path);
    if (!f.is_open()) {
        return Json{};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const auto parsed = json::parse(ss.str());
    return parsed ? std::move(parsed.value()) : Json{};
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

}  // namespace aurora::tools
