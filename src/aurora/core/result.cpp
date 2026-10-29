#include "aurora/core/result.h"

namespace aurora {

auto Error::to_json() const -> std::string {
    auto jstr = [](const std::string &s) -> std::string {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        out += '"';
        return out;
    };
    std::string out = "{";
    out += "\"code\":" + jstr(code) + ",";
    out += "\"message\":" + jstr(message);
    if (!suggestion.empty()) {
        out += ",\"suggestion\":" + jstr(suggestion);
    }
    if (!docs.empty()) {
        out += ",\"docs\":" + jstr(docs);
    }
    if (!where.empty()) {
        out += ",\"where\":" + jstr(where);
    }
    if (!hint.empty()) {
        out += ",\"hint\":" + jstr(hint);
    }
    // 编译期枚举码 + 表驱动元数据
    out += ",\"code_enum\":" + jstr(std::string(to_string(code_enum)));
    out += ",\"severity\":" + jstr(std::string(to_string(severity)));
    out += ",\"domain\":" + jstr(std::string(to_string(domain)));
    out += ",\"kind\":" + jstr(std::string(to_string(kind)));
    out += ",\"retryable\":" + std::string(retryable ? "true" : "false");
    out += '}';
    return out;
}

}  // namespace aurora
