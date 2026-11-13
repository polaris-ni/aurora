#include "aurora/app/detail/shell_integration_translate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/core/platform.h"

namespace aurora::desktop_shell {

namespace {

/// 保留方案名单：这些方案属于浏览器 / 邮件 / 本地文件等既有生态，应用不得注册抢占。
/// 名单只影响**注册**判定；命令行里显式传入的 URI 仍按文法放行（见 extract_uri_argument）。
constexpr std::array<std::string_view, 18> AURORA_RESERVED_SCHEMES = {
    "http",       "https", "file", "mailto", "ftp", "ftps", "tel",  "sms",    "data",
    "javascript", "about", "ws",   "wss",    "ssh", "urn",  "ldap", "gopher", "news",
};

auto ascii_lower(std::string_view text) -> std::string {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
    });
    return out;
}

[[nodiscard]] constexpr auto is_ascii_alpha(char ch) -> bool {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

[[nodiscard]] constexpr auto is_ascii_digit(char ch) -> bool { return ch >= '0' && ch <= '9'; }

/// @brief RFC 3986 方案文法：`ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`。
[[nodiscard]] auto scheme_syntax_ok(std::string_view scheme) -> bool {
    if (scheme.empty() || !is_ascii_alpha(scheme.front())) {
        return false;
    }
    return std::ranges::all_of(scheme.substr(1), [](char ch) {
        return is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '+' || ch == '-' || ch == '.';
    });
}

[[nodiscard]] auto is_path_separator(char ch) -> bool { return ch == '/' || ch == '\\'; }

/// @brief 词法规范化路径：分隔符统一 `/`、消解 `.` / `..`、折叠重复分隔符。
/// @param path 原始路径。
/// @param fold_case 是否做 ASCII 小写折叠（Windows / macOS 的路径大小写不敏感）。
/// @return 规范化后的路径文本。
[[nodiscard]] auto normalize_path_lexical(std::string_view path, bool fold_case) -> std::string {
    std::string slashed;
    slashed.reserve(path.size());
    for (const char ch : path) {
        slashed.push_back(is_path_separator(ch) ? '/' : ch);
    }
    if (fold_case) {
        slashed = ascii_lower(slashed);
    }
    const bool absolute = !slashed.empty() && slashed.front() == '/';
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= slashed.size()) {
        const auto end = slashed.find('/', start);
        const auto piece =
            std::string_view(slashed).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (piece.empty() || piece == ".") {
            // 空段（重复分隔符）与当前目录段都不进栈。
        } else if (piece == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                segments.push_back(piece);  // 相对路径开头的 .. 保留（无法再消解）
            }
        } else {
            segments.push_back(piece);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            out.push_back('/');
        }
        out.append(segments[index]);
    }
    return out;
}

/// @brief FNV-1a 64 位摘要（实例键用）。
[[nodiscard]] auto fnv1a_64(std::string_view text) -> std::uint64_t {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] auto trim(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

auto normalize_url_scheme(std::string_view scheme) -> std::string { return ascii_lower(trim(scheme)); }

auto is_reserved_scheme(std::string_view scheme) -> bool {
    const auto lower = ascii_lower(trim(scheme));
    return std::ranges::any_of(AURORA_RESERVED_SCHEMES,
                               [&lower](std::string_view reserved) { return reserved == lower; });
}

auto check_url_scheme(std::string_view scheme) -> SchemeCheck {
    const auto value = trim(scheme);
    if (value.empty()) {
        return SchemeCheck::Empty;
    }
    if (!scheme_syntax_ok(value)) {
        return SchemeCheck::InvalidSyntax;
    }
    return is_reserved_scheme(value) ? SchemeCheck::Reserved : SchemeCheck::Valid;
}

auto normalize_extension(std::string_view extension) -> std::optional<std::string> {
    auto value = trim(extension);
    if (!value.empty() && value.front() == '.') {
        value.remove_prefix(1);
    }
    if (value.empty() || value == ".") {
        return std::nullopt;
    }
    std::string out;
    out.reserve(value.size());
    for (const unsigned char raw : value) {
        const char ch = static_cast<char>(raw);
        if (is_path_separator(ch) || ch == '.' || ch == ' ' || ch == '\t' || ch == ':' || ch < 0x21 || ch == 0x7F) {
            return std::nullopt;  // 路径分隔符 / 内嵌点 / 空白 / 控制字符都不是合法扩展名
        }
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out;
}

auto extension_to_mime(std::string_view extension) -> std::optional<std::string_view> {
    return mime_type_for_extension(extension);
}

auto derive_instance_key(std::string_view executable_path) -> std::string {
    std::string path(executable_path);
    if (!path.empty()) {
        // 符号链接解析到同一实录路径：只有真实存在的路径才 canonical；失败时退回词法规范化。
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
        if (!ec) {
            path = canonical.string();
        }
    }
#if defined(AURORA_PLATFORM_WINDOWS) || defined(AURORA_PLATFORM_MACOS)
    // 两个平台的默认文件系统大小写不敏感；其余平台保留原样。
    constexpr bool fold_case = true;
#else
    constexpr bool fold_case = false;
#endif
    const auto normalized = normalize_path_lexical(path, fold_case);
    const auto hash = fnv1a_64(normalized);
    constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string key(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        key[15 - index] = digits.at((hash >> (index * 4U)) & 0xFU);
    }
    return key;
}

auto extract_uri_argument(const std::vector<std::string> &arguments) -> std::optional<std::string> {
    for (const auto &argument : arguments) {
        if (argument.size() < 4 || argument.front() == '-') {
            continue;  // 太短 / 选项参数不是 URI
        }
        const auto colon = argument.find(':');
        if (colon == std::string::npos || colon < 2) {
            continue;  // 无冒号，或单字母方案（Windows 盘符 `C:`）不当作 URI
        }
        const std::string_view scheme(argument.data(), colon);
        if (!scheme_syntax_ok(scheme)) {
            continue;
        }
        const char next = argument[colon + 1];
        if (next == '\\') {
            continue;  // `C:\...` 形态：盘符路径而非 URI
        }
        return argument;
    }
    return std::nullopt;
}

auto quote_exec_argument(std::string_view argument) -> std::string {
    // Desktop Entry 规范：Exec 中字面 `%` 必须写成 `%%`（否则会被当作字段码）。
    std::string escaped;
    escaped.reserve(argument.size() + 2);
    for (const char ch : argument) {
        if (ch == '%') {
            escaped.append("%%");
        } else {
            escaped.push_back(ch);
        }
    }
    const bool needs_quote = escaped.empty() || std::ranges::any_of(escaped, [](char ch) {
                                 return ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '\\' || ch == '$' ||
                                        ch == '`' || ch == ';' || ch == '&' || ch == '|';
                             });
    if (!needs_quote) {
        return escaped;
    }
    std::string out = "\"";
    for (const char ch : escaped) {
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

auto build_desktop_entry(const DesktopEntry &entry) -> std::string {
    std::string out;
    out.append("[Desktop Entry]\n");
    out.append("Type=Application\n");
    out.append("Name=");
    out.append(entry.name.empty() ? "Aurora Application" : entry.name);
    out.push_back('\n');
    if (!entry.comment.empty()) {
        out.append("Comment=");
        out.append(entry.comment);
        out.push_back('\n');
    }
    out.append("Exec=");
    out.append(entry.exec);
    out.push_back('\n');
    if (!entry.icon.empty()) {
        out.append("Icon=");
        out.append(entry.icon);
        out.push_back('\n');
    }
    if (!entry.mime_types.empty()) {
        out.append("MimeType=");
        out.append(entry.mime_types);
        out.push_back('\n');
    }
    if (!entry.url_scheme.empty()) {
        out.append("X-Aurora-Url-Scheme=");
        out.append(entry.url_scheme);
        out.push_back('\n');
    }
    out.append("Terminal=false\n");
    out.append(entry.hidden ? "Hidden=true\n" : "Hidden=false\n");
    if (entry.autostart) {
        out.append("X-GNOME-Autostart-enabled=");
        out.append(entry.hidden ? "false\n" : "true\n");
    }
    return out;
}

auto desktop_entry_value(std::string_view text, std::string_view key) -> std::optional<std::string> {
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line =
            trim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!line.empty() && line.front() != '#' && line.front() != '[') {
            const auto equal = line.find('=');
            if (equal != std::string_view::npos && trim(line.substr(0, equal)) == key) {
                return std::string(trim(line.substr(equal + 1)));
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

auto desktop_entry_disabled(std::string_view text) -> bool {
    if (const auto hidden = desktop_entry_value(text, "Hidden"); hidden.has_value() && ascii_lower(*hidden) == "true") {
        return true;
    }
    if (const auto enabled = desktop_entry_value(text, "X-GNOME-Autostart-enabled");
        enabled.has_value() && ascii_lower(*enabled) == "false") {
        return true;
    }
    return false;
}

auto desktop_entry_set_value(std::string_view text, std::string_view key, std::string_view value) -> std::string {
    std::string out;
    out.reserve(text.size() + value.size() + 16);
    bool written = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        const auto body = trim(line);
        const bool is_key_line = body.size() > key.size() && body.starts_with(key) && body[key.size()] == '=';
        if (!written && is_key_line) {
            if (!value.empty()) {
                out.append(key);
                out.push_back('=');
                out.append(value);
                out.push_back('\n');
            }
            written = true;
        } else if (!(written && is_key_line)) {
            out.append(line);
            out.push_back('\n');
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (!written && !value.empty()) {
        out.append(key);
        out.push_back('=');
        out.append(value);
        out.push_back('\n');
    }
    return out;
}

auto ini_value(std::string_view text, std::string_view section, std::string_view key) -> std::optional<std::string> {
    bool in_section = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto line =
            trim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            in_section = trim(line.substr(1, line.size() - 2)) == section;
        } else if (in_section && !line.empty() && line.front() != '#' && line.front() != ';') {
            const auto equal = line.find('=');
            if (equal != std::string_view::npos && trim(line.substr(0, equal)) == key) {
                return std::string(trim(line.substr(equal + 1)));
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

auto ini_set(std::string_view text, std::string_view section, std::string_view key, std::string_view value)
    -> std::string {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line =
            std::string(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    const auto body_is_key = [key](std::string_view body) -> bool {
        const auto trimmed = trim(body);
        const auto equal = trimmed.find('=');
        return equal != std::string_view::npos && trim(trimmed.substr(0, equal)) == key;
    };

    std::size_t section_begin = lines.size();
    std::size_t section_end = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto body = trim(lines[index]);
        if (body.size() >= 2 && body.front() == '[' && body.back() == ']') {
            if (trim(body.substr(1, body.size() - 2)) == section) {
                section_begin = index + 1;
                section_end = lines.size();
                for (std::size_t next = index + 1; next < lines.size(); ++next) {
                    const auto next_body = trim(lines[next]);
                    if (next_body.size() >= 2 && next_body.front() == '[' && next_body.back() == ']') {
                        section_end = next;
                        break;
                    }
                }
                break;
            }
        }
    }

    if (section_begin == lines.size()) {
        if (value.empty()) {
            return std::string(text);  // 无该节也无该键：删除是空操作
        }
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.push_back("[" + std::string(section) + "]");
        lines.push_back(std::string(key) + "=" + std::string(value));
    } else {
        bool written = false;
        for (std::size_t index = section_begin; index < section_end;) {
            if (!body_is_key(lines[index])) {
                ++index;
                continue;
            }
            if (!written && !value.empty()) {
                lines[index] = std::string(key) + "=" + std::string(value);
                written = true;
                ++index;
            } else {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                --section_end;
            }
        }
        if (!written && !value.empty()) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_end),
                         std::string(key) + "=" + std::string(value));
        }
    }

    std::string out;
    for (const auto &line : lines) {
        out.append(line);
        out.push_back('\n');
    }
    return out;
}

namespace {

/// @brief 拆分分号分隔列表（去空项、去首尾空白、保持顺序）。
[[nodiscard]] auto split_list(std::string_view list) -> std::vector<std::string> {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = list.find(';', start);
        const auto piece =
            trim(list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!piece.empty()) {
            items.emplace_back(piece);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return items;
}

/// @brief 拼回分号分隔列表（恒以 `;` 结尾）。
[[nodiscard]] auto join_list(const std::vector<std::string> &items) -> std::string {
    std::string out;
    for (const auto &item : items) {
        out.append(item);
        out.push_back(';');
    }
    return out;
}

}  // namespace

auto list_add(std::string_view list, std::string_view item) -> std::string {
    auto items = split_list(list);
    // `item` 也按分号切分：调用方从既有列表里原样搬来一项（常带结尾 `;`）时不得拼出 `;;`。
    for (const auto &candidate : split_list(item)) {
        if (std::ranges::none_of(items, [&candidate](const std::string &existing) { return existing == candidate; })) {
            items.push_back(candidate);
        }
    }
    return join_list(items);
}

auto list_remove(std::string_view list, std::string_view item) -> std::string {
    auto items = split_list(list);
    const auto unwanted = trim(item);
    std::erase(items, std::string(unwanted));
    return join_list(items);
}

auto list_contains(std::string_view list, std::string_view item) -> bool {
    const auto items = split_list(list);
    const auto wanted = trim(item);
    return std::ranges::any_of(items, [&wanted](const std::string &existing) { return existing == wanted; });
}

auto set_desktop_entry_hidden(std::string_view text, bool hidden) -> std::string {
    const std::string hidden_line = hidden ? "Hidden=true" : "Hidden=false";
    const std::string gnome_value = hidden ? "false" : "true";
    std::string out;
    out.reserve(text.size() + 32);
    bool hidden_written = false;
    bool gnome_seen = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        const auto body = trim(line);
        if (body.starts_with("Hidden=")) {
            out.append(hidden_line);
            hidden_written = true;
        } else if (body.starts_with("X-GNOME-Autostart-enabled=")) {
            out.append("X-GNOME-Autostart-enabled=");
            out.append(gnome_value);
            gnome_seen = true;
        } else {
            out.append(line);
        }
        out.push_back('\n');
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (!hidden_written) {
        out.append(hidden_line);
        out.push_back('\n');
    }
    // `gnome_seen` 仅用于保持“原文件带该键才同步”的口径；未出现时由 Hidden 承载禁用态。
    (void)gnome_seen;
    return out;
}

}  // namespace aurora::desktop_shell
