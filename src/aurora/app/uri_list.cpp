#include "aurora/app/uri_list.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::uri_list {

namespace {

/// @brief `file://` scheme 前缀（URI scheme 比较按规范大小写不敏感，故比对时先折小写）。
constexpr std::string_view AURORA_FILE_SCHEME = "file://";

/// @brief 判断字节是否属于「原样保留」集：字母数字与 `-._~/:`。
///
/// `:` 本属 URI 保留字符，但盘符（`C:\…`）与部分平台路径形态依赖它，且它出现在路径段内不改变
/// 解析结果，故一并保留——保留集越宽人眼可读性越好，代价只是解码侧必须认同一份集合。
[[nodiscard]] auto is_unreserved_byte(char c) -> bool {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

/// @brief 把 0..15 的半字节折成大写十六进制字符（与 URI 规范的推荐形一致）。
///
/// 用算术代替查表：查表就得用运行时下标索引常量数组，而下标范围靠 `& 0x0F` 保证，
/// 这一保证是给检查器看的注释而不是类型——算术版把同样的事实写进表达式，无需额外声明。
[[nodiscard]] auto hex_digit(std::uint8_t nibble) -> char {
    return static_cast<char>(nibble < 10U ? ('0' + nibble) : ('A' + (nibble - 10U)));
}

/// @brief 把单个字节折成 `%XX`（大写十六进制）追加进输出。
auto append_percent_encoded(std::string &out, char c) -> void {
    const auto byte = static_cast<std::uint8_t>(c);
    out.push_back('%');
    out.push_back(hex_digit(static_cast<std::uint8_t>((byte >> 4U) & 0x0FU)));
    out.push_back(hex_digit(static_cast<std::uint8_t>(byte & 0x0FU)));
}

/// @brief 把一个十六进制字符折成 4 位值。
/// @return 字符是合法十六进制数字时为对应半字节值；否则为空（`%` 后不是转义序列，按字面量保留）。
[[nodiscard]] auto hex_nibble(char c) -> std::optional<std::uint8_t> {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return std::nullopt;
}

/// @brief 判断一行是否为 `file://` 开头的条目（scheme 大小写不敏感）。
[[nodiscard]] auto has_file_scheme(std::string_view line) -> bool {
    if (line.size() < AURORA_FILE_SCHEME.size()) {
        return false;
    }
    for (std::size_t i = 0; i < AURORA_FILE_SCHEME.size(); ++i) {
        const char c = line[i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != AURORA_FILE_SCHEME[i]) {
            return false;
        }
    }
    return true;
}

/// @brief 按 CRLF / LF / CR 三种分隔符把载荷切成原始行，忽略空行。
///
/// 三种分隔符一律按单字符推进：把 CRLF 特判成「一个两字符分隔符」会在 CR 单独出现时漏切，而
/// 「CR 单独作分隔符」正是老式 Mac 文本与部分剪贴板实现的现实形态；末行行尾的分隔符与载荷
/// 以分隔符结尾的情形由此都不产生幽灵条目。
[[nodiscard]] auto split_lines(std::string_view payload) -> std::vector<std::string_view> {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const char c = payload[i];
        if (c == '\n' || c == '\r') {
            if (i > start) {
                lines.emplace_back(payload.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    if (payload.size() > start) {
        lines.emplace_back(payload.substr(start));
    }
    return lines;
}

}  // namespace

auto encode(const std::vector<std::string> &paths) -> std::string {
    std::string out;
    for (const std::string &path : paths) {
        if (path.empty()) {
            continue;  // 空路径不产出条目：它没有可编码内容，产出的空 URI 行同样没有意义
        }
        out += AURORA_FILE_SCHEME;
        // 空 authority 的第三个斜杠：`file://` + 绝对路径。Windows 盘符路径不以 `/` 开头，
        // 补上它才是资源管理器 / Nautilus 认的合法 file URI（缺了会被读侧当相对路径或解析失败）。
        if (!path.empty() && path.front() != '/') {
            out.push_back('/');
        }
        for (const char c : path) {
            if (is_unreserved_byte(c)) {
                out.push_back(c);
            } else {
                append_percent_encoded(out, c);
            }
        }
        out += "\r\n";  // text/uri-list 的行分隔符，末行同样带（读侧忽略随之产生的空行）
    }
    return out;
}

auto decode(std::string_view payload) -> std::vector<std::string> {
    std::vector<std::string> paths;
    for (const std::string_view line : split_lines(payload)) {
        if (!has_file_scheme(line)) {
            continue;  // 非 file scheme 的行（http:// 等）不映射为本地路径，静默跳过
        }
        std::string path;
        path.reserve(line.size() - AURORA_FILE_SCHEME.size());
        for (std::size_t i = AURORA_FILE_SCHEME.size(); i < line.size(); ++i) {
            if (line[i] == '%' && (i + 2U) < line.size()) {
                const auto high = hex_nibble(line[i + 1U]);
                const auto low = hex_nibble(line[i + 2U]);
                if (high && low) {
                    path.push_back(static_cast<char>(static_cast<std::uint8_t>(((*high) << 4U) | (*low))));
                    i += 2U;
                    continue;
                }
            }
            path.push_back(line[i]);
        }
        // 盘符路径还原：`file:///C:/x` 剥掉 scheme 后是 `/C:/x`，那一个前导 `/` 是空 authority 的
        // 产物而非路径的一部分，剥掉它才回到 `C:/x`；POSIX 绝对路径（「/」开头且非盘符形）的前导 `/` 是真路径，保留。
        if (path.size() >= 3U && path[0] == '/' && path[2] == ':' &&
            ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z'))) {
            path.erase(path.begin());
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

}  // namespace aurora::uri_list
