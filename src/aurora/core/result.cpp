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
    out += ",\"category\":" + jstr(std::string(to_string(category)));
    out += ",\"auto_fixable\":" + std::string(auto_fixable ? "true" : "false");
    out += ",\"retryable\":" + std::string(retryable ? "true" : "false");
    if (!fix_category.empty()) {
        out += ",\"fix_category\":" + jstr(fix_category);
    }
    if (!fix_params.empty()) {
        out += ",\"fix_params\":" + fix_params;  // fix_params 已是 JSON 字符串
    }
    out += '}';
    return out;
}

}  // namespace aurora
