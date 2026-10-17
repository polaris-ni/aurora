// Aurora core — URL 值类型实现（§12 基础件：RFC 3986 语法、百分号编码、相对解析、IDNA punycode）。
//
// 与 net/address.cpp 同一纪律：**手写、不经平台 API**，让同一输入在 Win32 / POSIX / wasm 上得到
// 同一结果与同一条诊断（跨平台一致行为判据见 codespec/specification/06-app-platform.md §12.2）。
// 交给 Windows 的 `UrlEscape` / libc 的 `inet_pton` 就等于把「哪些畸形串被谁容忍」交给平台实现，
// 那正是三套行为分叉的起点。
//
// punycode 严格按 RFC 3492 §6.2 / §6.3 的两段伪码逐行落地（含其溢出检查），因为该算法的每一项都可判定：
// 解码结果与既有注册名不一致会直接产出「两个不同域名」的解析错误，静默截断比报错危险得多。
// ASCII 侧只做小写映射，**不做 Unicode 归一化与大小写折叠**——口径与其边界记在
// codespec/specification/01-core.md §11.5。

#include "aurora/core/url.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/utf8.h"

namespace aurora {
namespace {

/// @brief 各分量「除无保留字符外还允许直接出现」的附加字符谓词（决定百分号编码范围）。
using ExtraAllowed = bool (*)(unsigned char);

/// @brief 构造 `net-url-invalid`，把被拒原因写进 message（与 `net-endpoint-invalid` 同一形态）。
/// @param detail 人类可读的原因片段（ASCII，随输入文本回显）。
/// @return 表驱动 Error。
auto url_error(std::string detail) -> Error {
    return make_error(ErrorCode::NetUrlInvalid, "detail=" + std::move(detail));
}

/// @brief 单字节 ASCII 小写化；非大写字母原样返回（不经 locale 敏感的 `std::tolower`）。
/// @param c 原始字节。
/// @return 折叠后的字节。
constexpr auto lower_ascii(unsigned char c) -> unsigned char {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

/// @brief 把整段文本按 ASCII 小写化（非字母字节原样保留，含 UTF-8 续字节）。
/// @param text 输入文本。
/// @return 小写化文本。
auto lower_ascii(std::string_view text) -> std::string {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(lower_ascii(c)); });
    return out;
}

/// @brief 判断字节是否为 ASCII 字母。
/// @param c 原始字节。
/// @return 大小写拉丁字母之一时为 true。
constexpr auto is_alpha(unsigned char c) -> bool { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/// @brief 判断字节是否为 ASCII 数字。
/// @param c 原始字节。
/// @return '0'-'9' 之一时为 true。
constexpr auto is_digit(unsigned char c) -> bool { return c >= '0' && c <= '9'; }

/// @brief 判断字节是否为十六进制数字。
/// @param c 原始字节。
/// @return 0-9a-fA-F 之一时为 true。
constexpr auto is_hex(unsigned char c) -> bool {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// @brief 十六进制字符转数值。
/// @param c 十六进制数字（须先经 `is_hex` 判定）。
/// @return 0-15 的数值。
constexpr auto hex_value(unsigned char c) -> unsigned int {
    if (is_digit(c)) {
        return c - '0';
    }
    return lower_ascii(c) - 'a' + 10U;
}

/// @brief 无保留字符（RFC 3986 §2.3）：字母数字与 `-` `.` `_` `~`。
/// @param c 原始字节。
/// @return 属无保留集时为 true。
constexpr auto is_unreserved(unsigned char c) -> bool {
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

/// @brief 子界分隔符（RFC 3986 §2.2）：`!$&'()*+,;=`。
/// @param c 原始字节。
/// @return 属该 11 个字符之一时为 true。
constexpr auto is_sub_delim(unsigned char c) -> bool {
    return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ',' ||
           c == ';' || c == '=';
}

/// @brief 最严集合：只有无保留字符可不转义。
/// @param c 原始字节。
/// @return 恒为 false。
constexpr auto no_extra([[maybe_unused]] unsigned char c) -> bool { return false; }

/// @brief 路径段附加字符（pchar = unreserved / pct / sub-delims / `:` / `@`，再允许 `/`）。
/// @param c 原始字节。
/// @return 可不转义时为 true。
constexpr auto is_path_extra(unsigned char c) -> bool { return is_sub_delim(c) || c == ':' || c == '@' || c == '/'; }

/// @brief 查询串内**单个键或值**的附加字符：`&` 与 `=` 是该串的定界符，必须转义，
///        否则含 `&` 的值序列化后会被解成两个参数（多值表无法往返）。
/// @param c 原始字节。
/// @return 可不转义时为 true。
constexpr auto is_query_extra(unsigned char c) -> bool {
    return (is_sub_delim(c) && c != '&' && c != '=') || c == ':' || c == '@' || c == '/' || c == '?';
}

/// @brief **整段查询串**的附加字符（解析 / 构造用）：定界符在此处是结构而非数据，原样保留。
/// @param c 原始字节。
/// @return 可不转义时为 true。
constexpr auto is_whole_query_extra(unsigned char c) -> bool { return is_path_extra(c) || c == '?'; }

/// @brief 片段附加字符（pchar / `/` / `?`，再放 `#`：片段里的 `#` 不再充当定界符）。
/// @param c 原始字节。
/// @return 可不转义时为 true。
constexpr auto is_fragment_extra(unsigned char c) -> bool { return is_path_extra(c) || c == '?' || c == '#'; }

/// @brief 用户信息段附加字符（userinfo = unreserved / pct / sub-delims / `:`，RFC 3986 §3.2.1）。
/// @param c 原始字节。
/// @return 可不转义时为 true。
constexpr auto is_userinfo_extra(unsigned char c) -> bool { return is_sub_delim(c) || c == ':'; }

/// @brief 公共枚举到内部谓词的映射（`Unreserved` 走末路分支，保持闭合集无需默认分支）。
/// @param set 目标分量字符集。
/// @return 该集合的附加允许字符谓词。
auto extra_for(PercentSet set) -> ExtraAllowed {
    if (set == PercentSet::PathSegment) {
        return &is_path_extra;
    }
    if (set == PercentSet::QueryParam) {
        return &is_query_extra;
    }
    if (set == PercentSet::Fragment) {
        return &is_fragment_extra;
    }
    return &no_extra;
}

/// @brief 按附加谓词判断某字节是否需要转义。
/// @param c 原始字节。
/// @param extra 该分量的附加允许字符谓词。
/// @return 需转成百分号三元组时为 true。
constexpr auto must_escape(unsigned char c, ExtraAllowed extra) -> bool { return !is_unreserved(c) && !extra(c); }

/// @brief scheme token（RFC 3986 §3.1）：字母开头，后随字母数字与 `+` `-` `.`。
/// @param text 候选 token。
/// @return 合法时为 true；空串或含非法字符为 false。
auto is_scheme_token(std::string_view text) -> bool {
    if (text.empty() || !is_alpha(static_cast<unsigned char>(text.front()))) {
        return false;
    }
    return std::all_of(text.begin() + 1, text.end(), [](const char c) {
        const auto byte = static_cast<unsigned char>(c);
        return is_alpha(byte) || is_digit(byte) || byte == '+' || byte == '-' || byte == '.';
    });
}

/// @brief 取 `s[i]` 处的 UTF-8 序列长度（非法形态返回 0，绝不读越界）。
///
/// 不用 `utf8_cp_len()`：后者对非法首字节按 1 退化（渲染侧的宽容），而 URL 侧必须能拒绝
/// overlong、代理区与超过 U+10FFFF 的序列——静默接受会产出两种编码指同一码点的 URL。
/// @param s 输入文本。
/// @param i 待解码的字节下标。
/// @return 1/2/3/4，或 0 表示该处不是合法的 UTF-8 起点。
auto utf8_sequence_length(std::string_view s, std::size_t i) -> int {
    const auto first = static_cast<unsigned char>(s.at(i));
    const std::size_t remaining = s.size() - i;
    if (first < 0x80U) {
        return 1;
    }
    if ((first & 0xE0U) == 0xC0U) {
        const auto second = remaining >= 2U ? static_cast<unsigned char>(s.at(i + 1)) : 0U;
        return first >= 0xC2U && first <= 0xDFU && (second & 0xC0U) == 0x80U ? 2 : 0;
    }
    if ((first & 0xF0U) == 0xE0U) {
        if (remaining < 3U) {
            return 0;
        }
        const auto second = static_cast<unsigned char>(s.at(i + 1));
        const auto third = static_cast<unsigned char>(s.at(i + 2));
        const bool second_ok = (first == 0xE0U && second >= 0xA0U && second <= 0xBFU) ||
                               (first >= 0xE1U && first <= 0xECU && second >= 0x80U && second <= 0xBFU) ||
                               (first == 0xEDU && second >= 0x80U && second <= 0x9FU) ||
                               (first >= 0xEEU && first <= 0xEFU && second >= 0x80U && second <= 0xBFU);
        return second_ok && (third & 0xC0U) == 0x80U ? 3 : 0;
    }
    if ((first & 0xF8U) == 0xF0U) {
        if (remaining < 4U) {
            return 0;
        }
        const auto second = static_cast<unsigned char>(s.at(i + 1));
        const auto third = static_cast<unsigned char>(s.at(i + 2));
        const auto fourth = static_cast<unsigned char>(s.at(i + 3));
        const bool second_ok = (first == 0xF0U && second >= 0x90U && second <= 0xBFU) ||
                               (first >= 0xF1U && first <= 0xF3U && second >= 0x80U && second <= 0xBFU) ||
                               (first == 0xF4U && second >= 0x80U && second <= 0x8FU);
        return second_ok && (third & 0xC0U) == 0x80U && (fourth & 0xC0U) == 0x80U ? 4 : 0;
    }
    return 0;
}

/// @brief 解码一个合法 UTF-8 序列的码点（长度须已由 `utf8_sequence_length` 判定）。
/// @param s 输入文本。
/// @param i 首字节下标。
/// @param len 序列长度（1-4）。
/// @return Unicode 码点。
auto utf8_code_point(std::string_view s, std::size_t i, int len) -> std::uint32_t {
    const auto first = static_cast<unsigned char>(s.at(i));
    if (len == 1) {
        return first;
    }
    auto cp = static_cast<std::uint32_t>(first & (0xFFU >> static_cast<unsigned>(len + 1)));
    for (int k = 1; k < len; ++k) {
        cp = (cp << 6U) | (static_cast<unsigned char>(s.at(i + static_cast<std::size_t>(k))) & 0x3FU);
    }
    return cp;
}

/// @brief 严格切分 UTF-8：非法字节序列直接失败，不做替换字符兜底。
/// @param s 输入文本。
/// @param out 出参：码点序列。
/// @return 全部序列合法时 true。
auto split_code_points(std::string_view s, std::vector<std::uint32_t> &out) -> bool {
    for (std::size_t i = 0; i < s.size();) {
        const int len = utf8_sequence_length(s, i);
        if (len == 0) {
            return false;
        }
        out.push_back(utf8_code_point(s, i, len));
        i += static_cast<std::size_t>(len);
    }
    return true;
}

/// @brief 把字节落成两位大写的百分号三元组。
/// @param out 追加目标。
/// @param byte 待编码字节。
void append_pct(std::string &out, unsigned char byte) {
    static constexpr std::string_view HEX_TABLE = "0123456789ABCDEF";
    out.push_back('%');
    out.push_back(HEX_TABLE.at(byte >> 4U));
    out.push_back(HEX_TABLE.at(byte & 0x0FU));
}

/// @brief 按分量清洗**已在编码态**的文本：校验字符集、归一化百分号三元组、把裸非 ASCII 字节转成 UTF-8 三元组。
///
/// 三条口径值得写明，因为它们都曾是缺陷源：
/// - 控制字符与空格一律**拒收**而非转义——转义会把「粘贴带了换行」伪装成一个合法 URL；
/// - `%` 必须引出两个十六进制位，否则拒收（`%2` 与 `%zz` 都是坏输入，不是「保留原样」）；
/// - 无保留字符的三元组还原成字符（`%41` → `A`），其余三元组大写化（`%2f` → `%2F`），
///   于是同一资源的两种写法序列化后逐字节相同。
///
/// 解码态的输入不走这里，走 `percent_encode()`：那边的 `%` 是要转义的数据而非转义引导符。
/// @param text 分量原始文本（编码态）。
/// @param extra 该分量的附加允许字符谓词。
/// @param what 诊断用的分量名。
/// @return 清洗后的编码态文本；非法输入返回 `net-url-invalid`。
auto sanitize_component(std::string_view text, ExtraAllowed extra, std::string_view what) -> Result<std::string> {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text.at(i));
        if (c < 0x21U || c == 0x7FU) {
            return url_error(std::string(what) + " contains a control or space character at offset " +
                             std::to_string(i));
        }
        if (c >= 0x80U) {
            const int len = utf8_sequence_length(text, i);
            if (len == 0) {
                return url_error(std::string(what) + " has an invalid UTF-8 byte at offset " + std::to_string(i));
            }
            for (int k = 0; k < len; ++k) {
                append_pct(out, static_cast<unsigned char>(text.at(i + static_cast<std::size_t>(k))));
            }
            i += static_cast<std::size_t>(len - 1);
            continue;
        }
        if (c != '%') {
            if (must_escape(c, extra)) {
                append_pct(out, c);
            } else {
                out.push_back(static_cast<char>(c));
            }
            continue;
        }
        if (i + 2U >= text.size() || !is_hex(static_cast<unsigned char>(text.at(i + 1U))) ||
            !is_hex(static_cast<unsigned char>(text.at(i + 2U)))) {
            return url_error(std::string(what) + " has a truncated percent escape at offset " + std::to_string(i));
        }
        const unsigned int value = (hex_value(static_cast<unsigned char>(text.at(i + 1U))) * 16U) +
                                   hex_value(static_cast<unsigned char>(text.at(i + 2U)));
        if (is_unreserved(static_cast<unsigned char>(value))) {
            out.push_back(static_cast<char>(value));
        } else {
            append_pct(out, static_cast<unsigned char>(value));
        }
        i += 2U;
    }
    return Result<std::string>{std::move(out)};
}

/// @brief 单个注册名标签是否合法：ASCII 侧走 LDH，非 ASCII 侧验 UTF-8 序列完整性。
/// @param label 标签文本。
/// @return 合法时为 true；空标签、首尾连字符、非法字符、残缺 UTF-8 均拒。
auto label_is_valid(std::string_view label) -> bool {
    if (label.empty() || label.front() == '-' || label.back() == '-') {
        return false;
    }
    for (std::size_t i = 0; i < label.size(); ++i) {
        const auto c = static_cast<unsigned char>(label.at(i));
        if (c < 0x80U) {
            if (!is_alpha(c) && !is_digit(c) && c != '-') {
                return false;
            }
            continue;
        }
        const int len = utf8_sequence_length(label, i);
        if (len < 2) {
            return false;
        }
        i += static_cast<std::size_t>(len - 1);
    }
    return true;
}

// ---- RFC 3492 punycode ------------------------------------------------------

/// @brief punycode 参数集（RFC 3492 §5 常量表）。
struct PunycodeParams {
    /// @brief 进位基数。
    static constexpr std::uint32_t AURORA_BASE = 36U;
    /// @brief 最小阈值。
    static constexpr std::uint32_t AURORA_TMIN = 1U;
    /// @brief 最大阈值。
    static constexpr std::uint32_t AURORA_TMAX = 26U;
    /// @brief 偏差随机化参数。
    static constexpr std::uint32_t AURORA_SKEW = 38U;
    /// @brief 首次插入的衰减因子。
    static constexpr std::uint32_t AURORA_DAMP = 700U;
    /// @brief 初始偏差。
    static constexpr std::uint32_t AURORA_INITIAL_BIAS = 72U;
    /// @brief 初始码点窗口（ASCII 上界 +1）。
    static constexpr std::uint32_t AURORA_INITIAL_N = 0x80U;
    /// @brief 基本码点段与编码段的分隔符。
    static constexpr char AURORA_DELIMITER = '-';
    /// @brief Unicode 码点上界。
    static constexpr std::uint32_t AURORA_MAX_CODE_POINT = 0x10FFFFU;
    /// @brief IDNA 的 punycode 标签前缀。
    static constexpr std::string_view AURORA_PREFIX = "xn--";
};

/// @brief 把 0-35 的值落成 punycode 字母表字符。
/// @param value 数值（须小于基数）。
/// @return 字母表字符（0-25 → `a`-`z`，26-35 → `0`-`9`）。
constexpr auto punycode_digit(std::uint32_t value) -> char {
    return value < 26U ? static_cast<char>('a' + static_cast<unsigned char>(value))
                       : static_cast<char>('0' + static_cast<unsigned char>(value - 26U));
}

/// @brief punycode 字母表字符转数值（大小写不敏感，解码侧的宽容只限字母表）。
/// @param c 字母表字符。
/// @return 数值；非字母表字符为 `nullopt`。
constexpr auto punycode_digit_value(char c) -> std::optional<std::uint32_t> {
    const auto raw = static_cast<unsigned char>(c);
    if (raw >= 'a' && raw <= 'z') {
        return static_cast<std::uint32_t>(raw - 'a');
    }
    if (raw >= 'A' && raw <= 'Z') {
        return static_cast<std::uint32_t>(raw - 'A');
    }
    if (raw >= '0' && raw <= '9') {
        return static_cast<std::uint32_t>(raw - '0' + 26U);
    }
    return std::nullopt;
}

/// @brief 阈值 `t`：RFC 3492 §6.3 的 `max(tmin, min(tmax, k - bias))` 分段形态。
/// @param k 当前位权下标。
/// @param bias 当前偏差。
/// @return 该位阈值。
constexpr auto punycode_t(std::uint32_t k, std::uint32_t bias) -> std::uint32_t {
    if (k <= bias) {
        return PunycodeParams::AURORA_TMIN;
    }
    if (k >= bias + PunycodeParams::AURORA_TMAX) {
        return PunycodeParams::AURORA_TMAX;
    }
    return k - bias;
}

/// @brief 偏差自适应函数（RFC 3492 §6.1 `adapt`）。
/// @param delta 距上次插入的增量。
/// @param numpoints 已产出码点数。
/// @param first_time 是否首次插入（首次用 `damp` 而非除以 2）。
/// @return 新的偏差。
auto punycode_adapt(std::uint64_t delta, std::uint64_t numpoints, bool first_time) -> std::uint32_t {
    using P = PunycodeParams;
    delta = first_time ? delta / P::AURORA_DAMP : delta / 2U;
    delta += delta / numpoints;
    std::uint64_t k = 0U;
    while (delta > ((static_cast<std::uint64_t>(P::AURORA_BASE) - P::AURORA_TMIN) * P::AURORA_TMAX) / 2U) {
        delta /= static_cast<std::uint64_t>(P::AURORA_BASE) - P::AURORA_TMIN;
        k += P::AURORA_BASE;
    }
    return static_cast<std::uint32_t>(
        k + (((static_cast<std::uint64_t>(P::AURORA_BASE) - P::AURORA_TMIN + 1U) * delta) / (delta + P::AURORA_SKEW)));
}

/// @brief punycode 编码（RFC 3492 §6.3）。
/// @param code_points 标签码点序列。
/// @param out 出参：不含 `xn--` 前缀的编码结果。
/// @return 成功为 true；增量或位权溢出时 false（宁可拒收也不产出被截断的标签）。
auto punycode_encode(const std::vector<std::uint32_t> &code_points, std::string &out) -> bool {
    using P = PunycodeParams;
    constexpr std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max();
    std::string basic;
    for (const auto cp : code_points) {
        if (cp < P::AURORA_INITIAL_N) {
            basic.push_back(static_cast<char>(lower_ascii(static_cast<unsigned char>(cp))));
        }
    }
    out = basic;
    const std::size_t b = basic.size();
    std::size_t h = b;
    if (b > 0U) {
        out.push_back(P::AURORA_DELIMITER);
    }
    std::uint64_t n = P::AURORA_INITIAL_N;
    std::uint64_t delta = 0U;
    std::uint32_t bias = P::AURORA_INITIAL_BIAS;
    while (h < code_points.size()) {
        std::uint64_t m = max_value;
        for (const auto cp : code_points) {
            if (cp >= n && cp < m) {
                m = cp;
            }
        }
        if (m == max_value) {
            return false;
        }
        if (m - n > (max_value - delta) / (h + 1U)) {
            return false;  // 乘法前先判：回绕会得到一个假小值，把坏标签伪装成好标签
        }
        delta += (m - n) * (h + 1U);
        n = m;
        for (const auto cp : code_points) {
            if (cp < n) {
                ++delta;
                if (delta == 0U) {
                    return false;
                }
            }
            if (cp != n) {
                continue;
            }
            std::uint64_t q = delta;
            for (std::uint32_t k = P::AURORA_BASE;; k += P::AURORA_BASE) {
                const std::uint32_t t = punycode_t(k, bias);
                if (q < t) {
                    break;
                }
                out.push_back(punycode_digit(t + static_cast<std::uint32_t>((q - t) % (P::AURORA_BASE - t))));
                q = (q - t) / (P::AURORA_BASE - t);
            }
            if (q >= P::AURORA_BASE) {
                return false;  // 末位超出字母表：增量过大，产出不了合法标签
            }
            out.push_back(punycode_digit(static_cast<std::uint32_t>(q)));
            bias = punycode_adapt(delta, h + 1U, h == b);
            delta = 0U;
            ++h;
        }
        ++delta;
        ++n;
    }
    return true;
}

/// @brief punycode 解码（RFC 3492 §6.2）。
/// @param encoded 不含 `xn--` 前缀的编码文本。
/// @param code_points 出参：还原的码点序列。
/// @return 成功为 true；字母表非法、变长整数截断、码点越界或溢出时 false。
auto punycode_decode(std::string_view encoded, std::vector<std::uint32_t> &code_points) -> bool {
    using P = PunycodeParams;
    constexpr std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max();
    const std::size_t last_delim = encoded.find_last_of(P::AURORA_DELIMITER);
    const bool has_delimiter = last_delim != std::string_view::npos;
    const std::size_t basic_end = has_delimiter ? last_delim : 0U;
    for (std::size_t i = 0; i < basic_end; ++i) {
        const auto c = static_cast<unsigned char>(encoded.at(i));
        if (c >= P::AURORA_INITIAL_N) {
            return false;  // 基本码点段出现非 ASCII：不是合法 punycode 文本
        }
        code_points.push_back(c);
    }
    std::size_t pos = has_delimiter ? basic_end + 1U : 0U;
    std::uint64_t n = P::AURORA_INITIAL_N;
    std::uint64_t i = 0U;
    std::uint32_t bias = P::AURORA_INITIAL_BIAS;
    while (pos < encoded.size()) {
        const std::uint64_t oldi = i;
        std::uint64_t w = 1U;
        for (std::uint32_t k = P::AURORA_BASE;; k += P::AURORA_BASE) {
            if (pos >= encoded.size()) {
                return false;  // 变长整数被截断
            }
            const auto value = punycode_digit_value(encoded.at(pos));
            ++pos;
            if (!value) {
                return false;
            }
            if (*value > (max_value - i) / w) {
                return false;
            }
            i += *value * w;
            const std::uint32_t t = punycode_t(k, bias);
            if (*value < t) {
                break;
            }
            if (w > max_value / (P::AURORA_BASE - t)) {
                return false;
            }
            w *= P::AURORA_BASE - t;
        }
        const std::uint64_t length = code_points.size() + 1U;
        bias = punycode_adapt(i - oldi, length, oldi == 0U);
        if (i / length > max_value - n) {
            return false;
        }
        n += i / length;
        i %= length;
        if (n > P::AURORA_MAX_CODE_POINT) {
            return false;
        }
        code_points.insert(code_points.begin() + static_cast<std::ptrdiff_t>(i), static_cast<std::uint32_t>(n));
        ++i;
    }
    return true;
}

/// @brief `xn--` 标签的自校验：前缀之后必须有可解码的非空内容。
/// @param label 已小写化的 ASCII 标签。
/// @return 不是 `xn--` 标签时为 true；是且可完整解码时为 true；否则 false。
auto punycode_label_is_plausible(std::string_view label) -> bool {
    using P = PunycodeParams;
    if (!label.starts_with(P::AURORA_PREFIX)) {
        return true;
    }
    const std::string_view body = label.substr(P::AURORA_PREFIX.size());
    if (body.empty()) {
        return false;
    }
    std::vector<std::uint32_t> code_points;
    return punycode_decode(body, code_points);
}

/// @brief 注册名主机 → ASCII（IDNA 形态）：ASCII 标签小写，非 ASCII 标签 punycode 加 `xn--` 前缀。
/// @param host 主机文本（可为 UTF-8）。
/// @param out 出参：ASCII 形态主机名。
/// @return 全部标签合法时 true；否则 false（诊断由调用方统一落 `net-url-invalid`）。
auto host_to_ascii(std::string_view host, std::string &out) -> bool {
    using P = PunycodeParams;
    out.clear();
    // 尾点（根标签）是同一个名字的另一种拼写而非另一个名字：随 WHATWG URL 一并去掉，
    // 否则 `example.com` 与 `example.com.` 判不等，缓存键与相等判定都会分裂。
    if (host.ends_with('.')) {
        host = host.substr(0, host.size() - 1U);
    }
    std::string_view rest = host;
    while (!rest.empty()) {
        const std::size_t dot = rest.find('.');
        const std::string_view label = dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (!label_is_valid(label)) {
            return false;
        }
        const bool ascii_only =
            std::all_of(label.begin(), label.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
        if (ascii_only) {
            const std::string lowered = lower_ascii(label);
            if (!punycode_label_is_plausible(lowered)) {
                return false;
            }
            out += lowered;
        } else {
            std::vector<std::uint32_t> code_points;
            std::string encoded;
            if (!split_code_points(label, code_points) || !punycode_encode(code_points, encoded)) {
                return false;
            }
            out += P::AURORA_PREFIX;
            out += encoded;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        out.push_back('.');
        rest = rest.substr(dot + 1);
    }
    return !out.empty();
}

/// @brief ASCII 主机名 → Unicode 显示形态（`xn--` 标签解码回 UTF-8，其余原样）。
/// @param host ASCII 主机名。
/// @param out 出参：显示形态。
/// @return 全部标签可解码时 true。
auto host_to_unicode(std::string_view host, std::string &out) -> bool {
    using P = PunycodeParams;
    out.clear();
    std::string_view rest = host;
    while (!rest.empty()) {
        const std::size_t dot = rest.find('.');
        const std::string_view label = dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (label.starts_with(P::AURORA_PREFIX)) {
            std::vector<std::uint32_t> code_points;
            if (!punycode_decode(label.substr(P::AURORA_PREFIX.size()), code_points)) {
                return false;
            }
            for (const auto cp : code_points) {
                out += utf8_encode(cp);
            }
        } else {
            out.append(label);
        }
        if (dot == std::string_view::npos) {
            break;
        }
        out.push_back('.');
        rest = rest.substr(dot + 1);
    }
    return true;
}

/// @brief 判断主机名是否形如 IPv4 点分四段（仅形态判定，取值域由 `ipv4_is_valid` 把关）。
/// @param host 主机文本。
/// @return 四段纯数字且段间无空时 true。
auto looks_like_ipv4(std::string_view host) -> bool {
    int dots = 0;
    bool previous_was_dot = true;  // 首段不得为空
    for (const auto c : host) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte == '.') {
            if (previous_was_dot) {
                return false;
            }
            ++dots;
            previous_was_dot = true;
            continue;
        }
        if (!is_digit(byte)) {
            return false;
        }
        previous_was_dot = false;
    }
    return dots == 3 && !previous_was_dot;
}

/// @brief 校验 IPv4 点分四段的取值（与 `net::Endpoint` 同口径：禁前导零、每段 ≤ 255）。
/// @param host 主机文本。
/// @return 合法 IPv4 字面量时为 true。
auto ipv4_is_valid(std::string_view host) -> bool {
    std::size_t start = 0U;
    for (int part = 0; part < 4; ++part) {
        const std::size_t dot = host.find('.', start);
        const std::string_view octet =
            dot == std::string_view::npos ? host.substr(start) : host.substr(start, dot - start);
        if (octet.empty() || octet.size() > 3U || (octet.size() > 1U && octet.front() == '0')) {
            return false;
        }
        unsigned int value = 0U;
        for (const auto c : octet) {
            value = (value * 10U) + static_cast<unsigned int>(c - '0');
        }
        if (value > 255U) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return part == 3;
        }
        start = dot + 1U;
    }
    return start == host.size();  // 段数恰为 4：第四段之后不得再有内容
}

/// @brief 数出一段 IPv6 文本里的 16 位组个数（内嵌 IPv4 计 2 组）。
/// @param part 冒号分隔的段序列（不含 `::`）。
/// @param groups 累加出参。
/// @param ipv4_allowed 该段序列的末尾是否允许内嵌 IPv4。
/// @return 每段都是 1-4 位十六进制（或合法末段 IPv4）时 true。
auto count_ipv6_groups(std::string_view part, int &groups, bool ipv4_allowed) -> bool {
    std::size_t pos = 0U;
    while (pos < part.size()) {
        const std::size_t colon = part.find(':', pos);
        const std::string_view token =
            colon == std::string_view::npos ? part.substr(pos) : part.substr(pos, colon - pos);
        if (token.empty()) {
            return false;  // 段内出现相邻冒号
        }
        const bool is_last = colon == std::string_view::npos;
        if (token.find('.') != std::string_view::npos) {
            if (!ipv4_allowed || !is_last || !ipv4_is_valid(token)) {
                return false;
            }
            groups += 2;
            return true;
        }
        if (token.size() > 4U ||
            !std::all_of(token.begin(), token.end(), [](char c) { return is_hex(static_cast<unsigned char>(c)); })) {
            return false;
        }
        ++groups;
        if (is_last) {
            return true;
        }
        pos = colon + 1U;
    }
    return true;
}

/// @brief 校验 IPv6 字面量内容（不含方括号）。
///
/// 按 RFC 3986 §3.2.2 的 `IPv6address` 形态判定：十六进制段以 `:` 连接、`::` 压缩至多一处、
/// 尾段可为内嵌 IPv4、末尾可带 RFC 6874 的 `%25` 作用域标识。这里只守语法，不做 128 位
/// 字节化解析——那是 `net::Endpoint` 的职责，而 `core` 不依赖 `net`。
/// @param host 方括号内的文本。
/// @return 语法合法时为 true。
auto ipv6_is_valid(std::string_view host) -> bool {
    if (host.empty()) {
        return false;
    }
    std::string_view body = host;
    if (const std::size_t zone = body.find("%25"); zone != std::string_view::npos) {
        if (zone == 0U || zone + 3U >= body.size()) {
            return false;  // 作用域标识既不能打头也不能为空
        }
        const std::string_view id = body.substr(zone + 3U);
        if (!std::all_of(id.begin(), id.end(), [](char c) { return is_unreserved(static_cast<unsigned char>(c)); })) {
            return false;  // 作用域名之后不得再出现定界符
        }
        body = body.substr(0, zone);
    }
    const std::size_t compression = body.find("::");
    if (compression != std::string_view::npos && body.find("::", compression + 1U) != std::string_view::npos) {
        return false;  // 至多一处 `::`
    }
    const std::string_view head = compression == std::string_view::npos ? body : body.substr(0, compression);
    const std::string_view tail =
        compression == std::string_view::npos ? std::string_view{} : body.substr(compression + 2U);
    if (compression != std::string_view::npos && (head.ends_with(':') || tail.starts_with(':'))) {
        return false;  // `:::` 一类畸形
    }
    int groups = 0;
    if (!count_ipv6_groups(head, groups, compression == std::string_view::npos)) {
        return false;
    }
    if (compression != std::string_view::npos && !count_ipv6_groups(tail, groups, true)) {
        return false;
    }
    if (compression == std::string_view::npos) {
        return groups == 8;
    }
    return groups <= 6;  // `::` 至少代表一个被压缩的零段（内嵌 IPv4 已按 2 组计）
}

/// @brief 解析并校验主机段，产出规范化 ASCII 形态与类型。
/// @param text 主机原始文本（IPv6 含方括号）。
/// @param out_host 出参：规范化主机名（IPv6 不含方括号）。
/// @param out_kind 出参：主机类型。
/// @param what 诊断用的上下文名。
/// @return 成功为 OK；非法主机为 `net-url-invalid`。
auto parse_host(std::string_view text, std::string &out_host, UrlHostKind &out_kind, std::string_view what)
    -> Result<void> {
    if (text.empty()) {
        out_kind = UrlHostKind::Empty;
        out_host.clear();
        return Result<void>{};
    }
    if (text.front() == '[' || text.back() == ']') {
        if (!text.starts_with('[') || !text.ends_with(']') || text.size() < 2U) {
            return url_error(std::string(what) + ": IPv6 literal needs both brackets");
        }
        const std::string_view inner = text.substr(1, text.size() - 2U);
        if (!ipv6_is_valid(inner)) {
            return url_error(std::string(what) + ": IPv6 literal is malformed");
        }
        out_host = lower_ascii(inner);
        out_kind = UrlHostKind::Ipv6;
        return Result<void>{};
    }
    if (text.find('[') != std::string_view::npos || text.find(']') != std::string_view::npos) {
        return url_error(std::string(what) + ": brackets are only valid around an IPv6 literal");
    }
    if (looks_like_ipv4(text)) {
        if (!ipv4_is_valid(text)) {
            return url_error(std::string(what) + ": IPv4 octet out of range or zero-padded");
        }
        out_host = std::string(text);
        out_kind = UrlHostKind::Ipv4;
        return Result<void>{};
    }
    std::string ascii;
    if (!host_to_ascii(text, ascii)) {
        return url_error(std::string(what) + ": registered name has an invalid label");
    }
    out_host = std::move(ascii);
    out_kind = UrlHostKind::Name;
    return Result<void>{};
}

/// @brief 校验并取端口数值（空文本表示「写了 `:` 但没写端口」，归一为无端口）。
/// @param text 冒号之后的端口文本。
/// @param out 出参：端口值。
/// @return 合法为 OK；非十进制或超范围为 `net-url-invalid`。
auto parse_port(std::string_view text, std::optional<std::uint16_t> &out) -> Result<void> {
    out = std::nullopt;
    if (text.empty()) {
        return Result<void>{};
    }
    if (text.size() > 5U ||
        !std::all_of(text.begin(), text.end(), [](char c) { return is_digit(static_cast<unsigned char>(c)); })) {
        return url_error("port must be 1-5 decimal digits");
    }
    unsigned int value = 0U;
    for (const auto c : text) {
        value = (value * 10U) + static_cast<unsigned int>(c - '0');
    }
    if (value > 65535U) {
        return url_error("port out of range");
    }
    out = static_cast<std::uint16_t>(value);
    return Result<void>{};
}

/// @brief 授权段的解析结果（内部结构，避免自由函数触碰 `Url` 私有成员）。
struct AuthorityParts {
    std::string userinfo;  ///< 编码态用户信息
    std::string host;  ///< 规范化主机名（IPv6 不含方括号）
    std::optional<std::uint16_t> port;  ///< 显式端口
    UrlHostKind host_kind{UrlHostKind::Empty};  ///< 主机类型
};

/// @brief 解析授权段（user-info / host / port）。
/// @param text `//` 之后、路径定界符之前的原文。
/// @param out 出参：授权段各部分。
/// @return 成功为 OK；否则携带诊断。
auto parse_authority(std::string_view text, AuthorityParts &out) -> Result<void> {
    std::string_view host_port = text;
    if (const std::size_t at = text.find_last_of('@'); at != std::string_view::npos) {
        auto userinfo = sanitize_component(text.substr(0, at), &is_userinfo_extra, "userinfo");
        if (!userinfo) {
            return userinfo.error();
        }
        out.userinfo = std::move(userinfo.value());
        host_port = text.substr(at + 1U);
    }
    std::string_view host_text = host_port;
    std::string_view port_text;
    if (host_port.starts_with('[')) {
        const std::size_t close = host_port.find(']');
        if (close == std::string_view::npos) {
            return url_error("authority is missing the closing bracket of the IPv6 literal");
        }
        host_text = host_port.substr(0, close + 1U);
        const std::string_view trailing = host_port.substr(close + 1U);
        if (trailing.empty()) {
            port_text = std::string_view{};
        } else if (!trailing.starts_with(':')) {
            return url_error("authority has trailing characters after the IPv6 literal");
        } else {
            port_text = trailing.substr(1);
        }
    } else if (const std::size_t colon = host_port.rfind(':'); colon != std::string_view::npos) {
        host_text = host_port.substr(0, colon);
        port_text = host_port.substr(colon + 1U);
    }
    auto port = std::optional<std::uint16_t>{};
    auto parsed_port = parse_port(port_text, port);
    if (!parsed_port) {
        return parsed_port.error();
    }
    auto host_result = parse_host(host_text, out.host, out.host_kind, "host");
    if (!host_result) {
        return host_result.error();
    }
    out.port = port;
    return Result<void>{};
}

/// @brief 去掉输出缓冲的最后一段（RFC 3986 §5.2.4 的 `/../` 处理）。
/// @param io 就地修改的输出缓冲。
void remove_last_segment(std::string &io) {
    const std::size_t slash = io.find_last_of('/');
    io.erase(slash == std::string::npos ? 0U : slash);
}

/// @brief 点段消除（RFC 3986 §5.2.4，照该节伪码的分支实现（RFC 的 `./` 与 `/./` 两条在本实现里同效，故合并为一支））。
///
/// 本函数只在传入的文本上匹配字面 `.` / `..`：`%2E%2E` 会不会走到这里变成点段，取决于上游
/// `sanitize_component` 先把无保留字符还原成 `.`（本库口径：还原，与 WHATWG 同色），而 `%2F`
/// 不属无保留集、永不还原成 `/`，故它不会凭空多出一个分隔段。
/// @param path 编码态路径。
/// @return 消除点段后的编码态路径。
auto remove_dot_segments(std::string_view path) -> std::string {
    std::string out;
    std::string_view in = path;
    while (!in.empty()) {
        if (in.starts_with("../")) {
            in.remove_prefix(3);
        } else if (in.starts_with("./") || in.starts_with("/./")) {
            in.remove_prefix(2);  // RFC 的两条分支在此同效："./x" → "x"、"/./x" → "/x"
        } else if (in == "/.") {
            in = std::string_view{"/", 1};
        } else if (in.starts_with("/../")) {
            in.remove_prefix(3);
            remove_last_segment(out);
        } else if (in == "/..") {
            in = std::string_view{"/", 1};
            remove_last_segment(out);
        } else if (in == "." || in == "..") {
            in = {};
        } else {
            const std::size_t next = in.find('/', in.front() == '/' ? 1U : 0U);
            const std::size_t len = next == std::string_view::npos ? in.size() : next;
            out.append(in, 0U, len);
            in.remove_prefix(len);
        }
    }
    return out;
}

/// @brief 拆分 scheme：返回小写 token 与其余部分。
/// @param text 输入文本。
/// @param out_scheme 出参：小写 scheme（不存在时为空）。
/// @param out_rest 出参：scheme 之后的部分（含 `:`）。
/// @return 文本里出现非法 scheme 形态（含 `:` 但前段不是合法 token）时 false。
auto split_scheme(std::string_view text, std::string &out_scheme, std::string_view &out_rest) -> bool {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text.at(i));
        if (c == ':') {
            const std::string_view token = text.substr(0, i);
            if (!is_scheme_token(token)) {
                return false;
            }
            out_scheme = lower_ascii(token);
            out_rest = text.substr(i + 1U);
            return true;
        }
        // 定界符先于冒号出现 => 整段是不含 scheme 的引用（RFC 3986 §4.2）。
        if (c == '/' || c == '?' || c == '#') {
            break;
        }
    }
    out_scheme.clear();
    out_rest = text;
    return true;
}

/// @brief 把不带前导 `/` 的引用路径与基址路径合并（RFC 3986 §5.2.3）。
/// @param base_has_authority 基址是否含授权段。
/// @param base_path 基址的编码态路径。
/// @param reference_path 引用的编码态路径。
/// @return 合并后的路径（点段未消除）。
auto merge_paths(bool base_has_authority, std::string_view base_path, std::string_view reference_path) -> std::string {
    if (base_has_authority && base_path.empty()) {
        return "/" + std::string(reference_path);
    }
    const std::size_t slash = base_path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::string(reference_path);
    }
    return std::string(base_path.substr(0, slash + 1U)) + std::string(reference_path);
}

/// @brief 默认端口表：只有这四个 scheme 有注册默认值。
/// @param scheme 小写协议名。
/// @return 默认端口；未知 scheme 为 `nullopt`。
auto default_port(std::string_view scheme) -> std::optional<std::uint16_t> {
    if (scheme == "http" || scheme == "ws") {
        return std::uint16_t{80};
    }
    if (scheme == "https" || scheme == "wss") {
        return std::uint16_t{443};
    }
    return std::nullopt;
}

}  // namespace

// ---- QueryParams ------------------------------------------------------------

auto QueryParams::append(std::string_view key, std::string_view value) -> void {
    entries_.push_back(Entry{.key = std::string(key), .value = std::string(value)});
}

auto QueryParams::set(std::string_view key, std::string_view value) -> void {
    std::erase_if(entries_, [key](const Entry &e) { return e.key == key; });
    append(key, value);
}

auto QueryParams::get(std::string_view key) const -> std::optional<std::string> {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [key](const Entry &e) { return e.key == key; });
    return it == entries_.end() ? std::nullopt : std::optional<std::string>(it->value);
}

auto QueryParams::values(std::string_view key) const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto &e : entries_) {
        if (e.key == key) {
            out.push_back(e.value);
        }
    }
    return out;
}

auto QueryParams::contains(std::string_view key) const -> bool {
    return std::any_of(entries_.begin(), entries_.end(), [key](const Entry &e) { return e.key == key; });
}

auto QueryParams::remove(std::string_view key) -> std::size_t {
    return std::erase_if(entries_, [key](const Entry &e) { return e.key == key; });
}

auto QueryParams::to_string() const -> std::string {
    std::string out;
    for (const auto &e : entries_) {
        if (!out.empty()) {
            out.push_back('&');
        }
        out += percent_encode(e.key, PercentSet::QueryParam);
        out.push_back('=');
        out += percent_encode(e.value, PercentSet::QueryParam);
    }
    return out;
}

auto QueryParams::operator==(const QueryParams &other) const -> bool {
    if (entries_.size() != other.entries_.size()) {
        return false;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key != other.entries_[i].key || entries_[i].value != other.entries_[i].value) {
            return false;
        }
    }
    return true;
}

// ---- percent 工具 -----------------------------------------------------------

auto percent_encode(std::string_view raw, PercentSet set) -> std::string {
    const ExtraAllowed extra = extra_for(set);
    std::string out;
    out.reserve(raw.size());
    for (const auto c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (must_escape(byte, extra)) {
            append_pct(out, byte);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

auto percent_decode(std::string_view encoded) -> Result<std::string> {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const auto c = static_cast<unsigned char>(encoded.at(i));
        if (c != '%') {
            out.push_back(encoded.at(i));
            continue;
        }
        if (i + 2U >= encoded.size() || !is_hex(static_cast<unsigned char>(encoded.at(i + 1U))) ||
            !is_hex(static_cast<unsigned char>(encoded.at(i + 2U)))) {
            return url_error("percent escape at offset " + std::to_string(i) + " is truncated");
        }
        const unsigned int value = (hex_value(static_cast<unsigned char>(encoded.at(i + 1U))) * 16U) +
                                   hex_value(static_cast<unsigned char>(encoded.at(i + 2U)));
        out.push_back(static_cast<char>(value));
        i += 2U;
    }
    return Result<std::string>{std::move(out)};
}

// ---- Url --------------------------------------------------------------------

auto Url::parse(std::string_view text) -> Result<Url> {
    Url url;
    std::string_view tail = text;

    // 先从尾部摘掉 fragment 与 query：两者内部允许已转义的定界符，故只能按首次出现的定界符切。
    if (const std::size_t hash = tail.find('#'); hash != std::string_view::npos) {
        auto fragment = sanitize_component(tail.substr(hash + 1U), &is_fragment_extra, "fragment");
        if (!fragment) {
            return fragment.error();
        }
        url.fragment_ = std::move(fragment.value());
        tail = tail.substr(0, hash);
    }
    if (const std::size_t mark = tail.find('?'); mark != std::string_view::npos) {
        auto query = sanitize_component(tail.substr(mark + 1U), &is_whole_query_extra, "query");
        if (!query) {
            return query.error();
        }
        url.query_ = std::move(query.value());
        tail = tail.substr(0, mark);
    }

    std::string_view rest = tail;
    if (!split_scheme(tail, url.scheme_, rest)) {
        return url_error("text before the first ':' is not a valid scheme token");
    }
    std::string_view path_text = rest;
    if (rest.starts_with("//")) {
        const std::size_t end = rest.find('/', 2U);
        AuthorityParts authority;
        auto parsed =
            parse_authority(end == std::string_view::npos ? rest.substr(2U) : rest.substr(2U, end - 2U), authority);
        if (!parsed) {
            return parsed.error();
        }
        url.userinfo_ = std::move(authority.userinfo);
        url.host_ = std::move(authority.host);
        url.host_kind_ = authority.host_kind;
        url.port_ = authority.port;
        url.has_authority_ = true;
        path_text = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    auto path = sanitize_component(path_text, &is_path_extra, "path");
    if (!path) {
        return path.error();
    }
    // 点段消除只作用于绝对 URL：RFC 3986 §5.2.4 把它定义在「引用与基址合并之后」，对未合并的
    // 引用提前消除会抹掉 `.` / `..` 携带的层级信息（`.` 会变成空路径并被基址覆盖）。
    url.path_ = url.scheme_.empty() ? std::move(path.value()) : remove_dot_segments(path.value());
    if (!url.scheme_.empty() && !url.has_authority_ && url.path_.starts_with("//")) {
        return url_error("a path that is not empty and begins with '//' needs an authority section");
    }
    return Result<Url>{std::move(url)};
}

auto Url::from_parts(std::string_view scheme, std::string_view host, std::optional<std::uint16_t> port,
                     std::string_view path, std::optional<std::string_view> query,
                     std::optional<std::string_view> fragment) -> Result<Url> {
    if (!is_scheme_token(scheme)) {
        return url_error("scheme is not a valid token");
    }
    Url url;
    url.scheme_ = lower_ascii(scheme);
    // 分量式调用方手上的 IPv6 常是裸地址（`::1`），而 URI 文本要求方括号形态（RFC 3986 §3.2.2）：
    // 这里补齐括号，`parse` 一侧仍保持严格，免得两处口径分叉成「谁加了括号谁负责」。
    std::string host_text(host);
    if (!host_text.empty() && host_text.front() != '[' && ipv6_is_valid(host_text)) {
        host_text = "[" + std::move(host_text) + "]";
    }
    auto host_result = parse_host(host_text, url.host_, url.host_kind_, "host");
    if (!host_result) {
        return host_result.error();
    }
    url.has_authority_ = !url.host_.empty() || port.has_value();
    url.port_ = port;
    if (url.has_authority_ && !path.empty() && path.front() != '/') {
        url.path_ = "/" + std::string(path);
    } else {
        url.path_ = std::string(path);
    }
    auto encoded_path = sanitize_component(url.path_, &is_path_extra, "path");
    if (!encoded_path) {
        return encoded_path.error();
    }
    url.path_ = remove_dot_segments(encoded_path.value());
    if (!url.has_authority_ && url.path_.starts_with("//")) {
        return url_error("a path that begins with '//' needs a host");
    }
    if (query) {
        auto encoded_query = sanitize_component(*query, &is_whole_query_extra, "query");
        if (!encoded_query) {
            return encoded_query.error();
        }
        url.query_ = std::move(encoded_query.value());
    }
    if (fragment) {
        auto encoded_fragment = sanitize_component(*fragment, &is_fragment_extra, "fragment");
        if (!encoded_fragment) {
            return encoded_fragment.error();
        }
        url.fragment_ = std::move(encoded_fragment.value());
    }
    return Result<Url>{std::move(url)};
}

auto Url::resolve(std::string_view reference) const -> Result<Url> {
    auto parsed = Url::parse(reference);
    if (!parsed) {
        return parsed.error();
    }
    Url out = std::move(parsed.value());
    if (out.is_absolute()) {
        return Result<Url>{std::move(out)};
    }
    if (!is_absolute()) {
        return url_error("a relative reference needs an absolute base URL");
    }
    out.scheme_ = scheme_;
    if (!out.has_authority_) {
        // 引用没写授权段时，基址的 userinfo / host / port 全部继承（RFC 3986 §5.2.2）。
        out.userinfo_ = userinfo_;
        out.host_ = host_;
        out.host_kind_ = host_kind_;
        out.port_ = port_;
        out.has_authority_ = has_authority_;
        if (out.path_.empty()) {
            out.path_ = path_;
            if (!out.query_.has_value()) {
                out.query_ = query_;
            }
        } else if (out.path_.front() != '/') {
            out.path_ = merge_paths(has_authority_, path_, out.path_);
        }
    }
    // 合并完成后才做点段消除（RFC 3986 §5.2.4），且对已规范化的绝对路径幂等。
    out.path_ = remove_dot_segments(out.path_);
    return Result<Url>{std::move(out)};
}

auto Url::effective_port() const -> std::optional<std::uint16_t> {
    return port_.has_value() ? port_ : default_port(scheme_);
}

auto Url::query_params() const -> Result<QueryParams> {
    QueryParams params;
    if (!query_) {
        return Result<QueryParams>{std::move(params)};
    }
    std::string_view rest = *query_;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view token = amp == std::string_view::npos ? rest : rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1U);
        if (token.empty()) {
            continue;  // `&&` 一类的空槽不产出幽灵参数
        }
        const std::size_t eq = token.find('=');
        auto key = percent_decode(eq == std::string_view::npos ? token : token.substr(0, eq));
        if (!key) {
            return key.error();
        }
        auto value = percent_decode(eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1U));
        if (!value) {
            return value.error();
        }
        params.append(key.value(), value.value());
    }
    return Result<QueryParams>{std::move(params)};
}

auto Url::to_string() const -> std::string {
    std::string out;
    if (!scheme_.empty()) {
        out += scheme_;
        out.push_back(':');
    }
    if (has_authority_) {
        out += "//";
        if (!userinfo_.empty()) {
            out += userinfo_;
            out.push_back('@');
        }
        if (host_kind_ == UrlHostKind::Ipv6) {
            out.push_back('[');
            out += host_;
            out.push_back(']');
        } else {
            out += host_;
        }
        if (port_) {
            out.push_back(':');
            out += std::to_string(*port_);
        }
    }
    out += path_;
    if (query_) {
        out.push_back('?');
        out += *query_;
    }
    if (fragment_) {
        out.push_back('#');
        out += *fragment_;
    }
    return out;
}

auto Url::to_unicode_host() const -> Result<std::string> {
    if (host_kind_ != UrlHostKind::Name) {
        return Result<std::string>{host_};
    }
    std::string out;
    if (!host_to_unicode(host_, out)) {
        return url_error("punycode label cannot be decoded for display");
    }
    return Result<std::string>{std::move(out)};
}

auto Url::operator==(const Url &other) const -> bool {
    return scheme_ == other.scheme_ && userinfo_ == other.userinfo_ && host_ == other.host_ &&
           host_kind_ == other.host_kind_ && port_ == other.port_ && path_ == other.path_ && query_ == other.query_ &&
           fragment_ == other.fragment_ && has_authority_ == other.has_authority_;
}

}  // namespace aurora
