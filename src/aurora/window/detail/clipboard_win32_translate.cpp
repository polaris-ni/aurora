#include "clipboard_win32_translate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::clipboard::detail {
namespace {

/// @brief `CF_HTML` 偏移字段的固定书写宽度（十进位零填充）。
///
/// 宽度恒定是「头部长度与片段长度无关」的前提——先按固定宽度算出头部长度，再回填四组数字。
constexpr std::size_t AURORA_HTML_OFFSET_DIGITS = 10U;

/// @brief `CF_HTML` 头部模板（四组偏移各占固定宽度，值随后就地覆写）。
constexpr std::string_view html_header_template() {
    return std::string_view{
        "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n"};
}

/// @brief 片段前缀（含 `<!--StartFragment -->` 标记）。
constexpr std::string_view html_prefix() { return std::string_view{"<html>\r\n<body>\r\n<!--StartFragment -->\r\n"}; }

/// @brief 片段后缀（含 `<!--EndFragment -->` 标记）。
constexpr std::string_view html_suffix() {
    return std::string_view{"\r\n<!--EndFragment -->\r\n</body>\r\n</html>\r\n"};
}

/// @brief 追加一个小端 `std::uint32_t`（逐字节写出，避免指针算术）。
auto append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) -> void {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/// @brief 追加一个 UTF-16 码元（小端两字节）。
auto append_utf16_unit(std::vector<std::uint8_t> &out, char16_t unit) -> void {
    out.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((unit >> 8) & 0xFFU));
}

/// @brief 读取一个小端 `std::uint32_t`；越界即返回 `std::nullopt`（跨进程字节不可信）。
[[nodiscard]] auto read_u32_at(const std::vector<std::uint8_t> &bytes, std::size_t at) -> std::optional<std::uint32_t> {
    if (at > bytes.size() || bytes.size() - at < sizeof(std::uint32_t)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        value |= static_cast<std::uint32_t>(bytes.at(at + i)) << (8U * i);
    }
    return value;
}

/// @brief 按固定宽度就地覆写十进位数字（长度不变）。
auto write_offset(std::string &out, std::size_t at, std::size_t value) -> void {
    for (std::size_t i = 0; i < AURORA_HTML_OFFSET_DIGITS; ++i) {
        out.at(at + AURORA_HTML_OFFSET_DIGITS - 1U - i) = static_cast<char>('0' + static_cast<int>(value % 10U));
        value /= 10U;
    }
}

/// @brief 在文本中取「某键名之后的十进位数值」；键名缺失或后面不是数字时为 `std::nullopt`。
[[nodiscard]] auto read_offset(const std::string &text, std::string_view key) -> std::optional<std::size_t> {
    const auto at = text.find(key);
    if (at == std::string::npos) {
        return std::nullopt;  // 头部缺项：不是 CF_HTML，交给调用方按畸形处理
    }
    std::size_t value = 0;
    std::size_t digits = 0;
    for (std::size_t i = at + key.size(); i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        value = (value * 10U) + static_cast<std::size_t>(text[i] - '0');
        ++digits;
    }
    return digits == 0U ? std::nullopt : std::optional<std::size_t>{value};
}

/// @brief 按字节偏移取一个 UTF-16 码元（小端组装，零指针算术）。
[[nodiscard]] auto unit_at(const std::vector<std::uint8_t> &bytes, std::size_t at) -> std::optional<char16_t> {
    if (at > bytes.size() || bytes.size() - at < 2U) {
        return std::nullopt;
    }
    const std::uint32_t low = bytes.at(at);
    const std::size_t high_offset = at + 1U;
    const std::uint32_t high = bytes.at(high_offset);
    return static_cast<char16_t>((high << 8U) | low);
}
}  // namespace

auto encode_hdrop(const std::vector<std::u16string> &paths) -> std::vector<std::uint8_t> {
    if (paths.empty()) {
        return {};  // 空清单无线上形态：与剪贴板侧「空清单为 no-op」同口径
    }
    std::vector<std::uint8_t> out;
    // `DROPFILES`：`pFiles`(4) + `pt`(8) + `fNC`(4) + `fWide`(4)，四字段皆 4 字节对齐、无填充。
    append_u32(out, static_cast<std::uint32_t>(AURORA_WIN32_DROPFILES_HEADER_BYTES));  // 清单起点
    append_u32(out, 0U);  // pt.x
    append_u32(out, 0U);  // pt.y
    append_u32(out, 0U);  // fNC
    append_u32(out, 1U);  // fWide
    for (const auto &path : paths) {
        for (const char16_t unit : path) {
            append_utf16_unit(out, unit);
        }
        append_utf16_unit(out, u'\0');  // 每条以 NUL 结尾
    }
    append_utf16_unit(out, u'\0');  // 清单整体再补一个 NUL（双空结尾）
    return out;
}

auto decode_hdrop(const std::vector<std::uint8_t> &bytes) -> std::optional<std::vector<std::u16string>> {
    if (bytes.size() < AURORA_WIN32_DROPFILES_HEADER_BYTES) {
        return std::nullopt;  // 连头部都不完整：不是 HDROP
    }
    const auto p_files = read_u32_at(bytes, 0U);
    const auto f_wide = read_u32_at(bytes, 16U);
    if (!p_files || *p_files < AURORA_WIN32_DROPFILES_HEADER_BYTES || *p_files > bytes.size() ||
        ((*p_files % 2U) != 0U)) {
        return std::nullopt;  // 清单起点越界或非偶：不猜偏移
    }
    if (f_wide != std::optional<std::uint32_t>{1U}) {
        return std::nullopt;  // ANSI 清单（fWide=0）按代码页解释字节，本库不猜代码页
    }
    const std::size_t available = bytes.size() - *p_files;
    const std::size_t count = available / 2U;
    if (count == 0U) {
        return std::nullopt;  // 清单区连一个宽字符都没有
    }
    // 清单**必须**以空字符收尾：最后一条路径若没有终止符，它的长度就无从判定——凭空补一个
    // NUL 会把不完整的路径变成一条看似完整的路径，那比「读不到」更糟。
    const auto terminator = unit_at(bytes, *p_files + ((count - 1U) * 2U));
    if (!terminator || *terminator != u'\0') {
        return std::nullopt;
    }
    std::vector<std::u16string> paths;
    std::size_t start = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto unit = unit_at(bytes, *p_files + (i * 2U));
        if (!unit || *unit != u'\0') {
            continue;
        }
        if (i > start) {
            std::u16string entry;
            for (std::size_t k = start; k < i; ++k) {
                const auto inner = unit_at(bytes, *p_files + (k * 2U));
                if (inner) {
                    entry.push_back(*inner);
                }
            }
            if (!entry.empty()) {
                paths.emplace_back(std::move(entry));
            }
        }
        start = i + 1U;
    }
    return paths;
}

auto build_html_frame(const std::string &fragment) -> std::string {
    const std::size_t start_html = html_header_template().size();
    const std::size_t start_fragment = start_html + html_prefix().size();
    const std::size_t end_fragment = start_fragment + fragment.size();
    const std::size_t end_html = end_fragment + html_suffix().size();
    std::string out{html_header_template()};
    write_offset(out, out.find("StartHTML:") + std::string_view{"StartHTML:"}.size(), start_html);
    write_offset(out, out.find("EndHTML:") + std::string_view{"EndHTML:"}.size(), end_html);
    write_offset(out, out.find("StartFragment:") + std::string_view{"StartFragment:"}.size(), start_fragment);
    write_offset(out, out.find("EndFragment:") + std::string_view{"EndFragment:"}.size(), end_fragment);
    out += html_prefix();
    out += fragment;
    out += html_suffix();
    return out;
}

auto parse_html_frame(const std::string &text) -> std::optional<std::string> {
    const auto start = read_offset(text, "StartFragment:");
    const auto end = read_offset(text, "EndFragment:");
    if (!start || !end) {
        return std::nullopt;  // 缺关键偏移：不是 CF_HTML
    }
    if (*start > *end || *end > text.size()) {
        return std::nullopt;  // 次序颠倒或越界：不切片
    }
    return text.substr(*start, *end - *start);
}

}  // namespace aurora::clipboard::detail
