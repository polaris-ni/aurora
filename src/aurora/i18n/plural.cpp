#include "aurora/i18n/plural.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/i18n/detail/plural_rules.h"

namespace aurora {
namespace {

/// @brief 取绝对值（INT64_MIN 安全：经无符号回转）。
auto abs_u64(std::int64_t v) -> unsigned long long {
    const auto u = static_cast<unsigned long long>(v);
    return v < 0 ? ~u + 1U : u;
}

/// @brief ASCII 小写化（语言码恒为 ASCII，逐字节映射）。
auto ascii_lower(std::string text) -> std::string {
    for (char &ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

/// @brief ASCII 大写化（地区码恒为 ASCII）。
auto ascii_upper(std::string text) -> std::string {
    for (char &ch : text) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return text;
}

/// @brief 书写系统子标签的规范形态（首字母大写，如 "latin" → "Latn"）。
auto ascii_title(std::string_view token) -> std::string {
    std::string out;
    out.reserve(token.size());
    for (std::size_t k = 0; k < token.size(); ++k) {
        const char ch = token[k];
        if (k == 0) {
            out.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
        } else {
            out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
    }
    return out;
}

/// @brief 规则集 id 的规范形态：按 `-`/`_` 分段，首段小写、4 字母段 Titlecase、2 字母段大写、其余小写。
///
/// CLDR 规则集 id 绝大多数只是语言码，少数带书写系统 / 地区的例外（`kok-Latn`、`pt-PT`）区分大小写，
/// 导出表按规范形态落键，故查表前把调用方给的任意大小写 / 分隔符形态先归一。
auto canonical_rule_key(std::string_view raw) -> std::string {
    std::string out;
    std::size_t pos = 0;
    int part = 0;
    while (pos <= raw.size()) {
        const std::size_t sep = raw.find_first_of("-_", pos);
        const std::size_t len = sep == std::string_view::npos ? raw.size() - pos : sep - pos;
        if (len > 0) {
            const std::string_view token = raw.substr(pos, len);
            if (!out.empty()) {
                out.push_back('-');  // 分隔符统一为 CLDR 惯用的连字符
            }
            if (part == 0 || token.size() == 3) {
                out += ascii_lower(std::string(token));  // 语言码与三段扩展子标签按小写
            } else if (token.size() == 4) {
                out += ascii_title(token);  // 书写系统：Latn / Hans / Cyrl
            } else {
                out += ascii_upper(std::string(token));  // 地区码与数字变体：PT / CN / 1901
            }
            ++part;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1;
    }
    return out;
}

/// @brief 组装规则表查找键：语言（可自带子标签）优先，再试 `语言-文字-地区` 逐级右删。
/// @param loc 区域设置。
/// @return 去重后的候选键，规范形态在前。
auto lookup_keys(const Locale &loc) -> std::vector<std::string> {
    const std::string base = canonical_rule_key(loc.language);
    const std::string script = loc.script.empty() ? std::string{} : "-" + ascii_title(loc.script);
    const std::string region = loc.region.empty() ? std::string{} : "-" + ascii_upper(loc.region);
    std::vector<std::string> keys;
    auto push = [&keys](std::string key) {
        if (key.empty() || std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(std::move(key));
        }
    };
    push(base);
    push(base + script + region);
    push(base + script);
    push(base + region);
    return keys;
}

/// @brief 按类型经规则表判定类别（键逐级尝试，未收录回 Other）。
auto category_via_rules(const PluralOperands &ops, const Locale &loc, bool ordinal) -> PluralCategory {
    for (const std::string &key : lookup_keys(loc)) {
        const int index = detail::plural_rules_language_index(key);
        if (index >= 0) {
            return ordinal ? detail::plural_rules_eval_ordinal(index, ops)
                           : detail::plural_rules_eval_cardinal(index, ops);
        }
    }
    return PluralCategory::Other;
}

}  // namespace

auto plural_operands_from_int(std::int64_t value) -> PluralOperands {
    PluralOperands ops;
    const auto mag = abs_u64(value);
    ops.i = static_cast<long long>(mag);
    ops.n = static_cast<double>(mag);
    return ops;
}

auto plural_operands_from_double(double value) -> PluralOperands {
    if (!std::isfinite(value)) {
        return PluralOperands{};  // NaN/Inf 无复数语义，按零值处理
    }
    char buf[48] = {};
    std::snprintf(buf, sizeof(buf), "%.15g", value);  // 最短可往返口径内的十进制文本
    if (std::strchr(buf, 'e') != nullptr || std::strchr(buf, 'E') != nullptr) {
        // 指数形态：小数位语义不可靠，退化为整数操作数（plural 选择场景的边缘输入）。
        return plural_operands_from_int(static_cast<std::int64_t>(value < 0 ? -std::trunc(-value) : std::trunc(value)));
    }
    return plural_operands_from_decimal(buf);
}

auto plural_operands_from_decimal(std::string_view text) -> PluralOperands {
    PluralOperands ops;
    std::size_t pos = 0;
    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        ++pos;
    }
    unsigned long long int_part = 0;
    bool any_digit = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        if (int_part <= 900000000000000000ULL) {  // 饱和防溢出（超出 18 位后仅保量级语义）
            int_part = (int_part * 10U) + static_cast<unsigned long long>(text[pos] - '0');
        }
        any_digit = true;
        ++pos;
    }
    unsigned long long frac = 0;
    int visible = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            any_digit = true;
            const int digit = text[pos] - '0';
            if (visible < 18) {  // 超长小数截断：v 计满后不再累积（TR35 判定只需前若干位）
                frac = (frac * 10U) + static_cast<unsigned long long>(digit);
                ++visible;
            }
            ++pos;
        }
    }
    if (!any_digit || pos != text.size()) {
        return PluralOperands{};  // 非十进制文本（分组符 / 字母 / 截断）→ 零值，与旧 to_double 回退口径一致
    }
    // 尾随零剥离：t/w 从 f/v 整体去零（"1.20" → t=2/w=1；"1.00" → t=0/w=0）。
    unsigned long long frac_trimmed = frac;
    int trimmed = visible;
    while (trimmed > 0 && frac_trimmed % 10U == 0) {
        frac_trimmed /= 10U;
        --trimmed;
    }
    // TR35：操作数一律取自绝对值，符号不参与分类。
    ops.i = static_cast<long long>(int_part);
    ops.v = visible;
    ops.w = trimmed;
    ops.f = static_cast<long long>(frac);
    ops.t = static_cast<long long>(frac_trimmed);
    const double magnitude =
        static_cast<double>(int_part) + (static_cast<double>(frac) / std::pow(10.0, static_cast<double>(visible)));
    ops.n = negative ? -magnitude : magnitude;
    return ops;
}

auto plural_category(const PluralOperands &ops, const Locale &loc) -> PluralCategory {
    return category_via_rules(ops, loc, false);
}

auto plural_category_ordinal(const PluralOperands &ops, const Locale &loc) -> PluralCategory {
    return category_via_rules(ops, loc, true);
}

auto plural_category(double n, const Locale &loc) -> PluralCategory {
    return plural_category(plural_operands_from_double(n), loc);
}

}  // namespace aurora
