// ============================================================================
// json_file.h — 文件读取原语（JSON / 原始文本）
// ----------------------------------------------------------------------------
// 零 aurora 依赖（仅标准库 + nlohmann/json）。由 aurora_cli / au-lint 复用，
// 避免「ifstream + rdbuf 读到 string/Json」两份重复实现。
//
// 注意：读取失败返回空值（Json{} / ""），不在此处打印日志 —— 调用方负责诊断，
// 与抽取前的行为一致（cli 在 read_json_file 返回后自判 is_discarded 再报错）。
// ============================================================================
#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace aurora::tools {

// 从文件读取并解析 JSON；失败（打不开 / 解析错）返回 discarded Json。
inline auto read_json_file(const std::string &path) -> nlohmann::json {
    std::ifstream f(path);
    if (!f.is_open()) {
        return nlohmann::json{};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str(), nullptr, false);
}

// 从文件读取原始文本；失败返回空串。
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
