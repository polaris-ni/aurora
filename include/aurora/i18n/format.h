#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "aurora/i18n/locale.h"

namespace aurora {

/**
 * @brief 货币代码（覆盖 Locale 完成判据所需：USD / JPY / EUR / CNY 等）。
 *
 * 符号与默认小数位取自 CLDR 轻量自研表（守零依赖）；后续可随需求扩展更多代码。
 *
 * @note Thread: thread-safe (value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
enum class Currency {
    USD,  ///< 美元（符号 $，默认 2 位小数）
    EUR,  ///< 欧元（符号 €，默认 2 位小数）
    JPY,  ///< 日元（符号 ¥，默认 0 位小数）
    CNY,  ///< 人民币（符号 ¥，默认 2 位小数）
    GBP,  ///< 英镑（符号 £，默认 2 位小数）
};

namespace detail {

/// @brief 某语言的千位分隔符 / 小数点 / 分组位数（自研轻量表，CLDR 简化版）。
struct NumberSymbols {
    char group;        ///< 千位分隔符
    char decimal;      ///< 小数点
    int group_size;    ///< 从右起的分组位数
};

[[nodiscard]] inline auto number_symbols(const std::string &lang) -> NumberSymbols {
    if (lang == "de" || lang == "ru") {
        return NumberSymbols{'.', ',', 3};  // 德语 / 俄语：点分组、逗号小数
    }
    if (lang == "fr") {
        return NumberSymbols{' ', ',', 3};  // 法语：空格分组（CLDR 真实用窄 NBSP U+202F）、逗号小数
    }
    return NumberSymbols{',', '.', 3};  // en / zh / ja / 其它：逗号分组、点小数
}

struct CurrencyInfo {
    const char *symbol;
    int default_fraction_digits;
};

[[nodiscard]] inline auto currency_info(Currency c) -> CurrencyInfo {
    switch (c) {
        case Currency::USD: return {"$", 2};
        case Currency::EUR: return {"\u20AC", 2};  // €
        case Currency::JPY: return {"\u00A5", 0};  // ¥
        case Currency::CNY: return {"\u00A5", 2};  // ¥
        case Currency::GBP: return {"\u00A3", 2};  // £
    }
    return {"$", 2};
}

/// @brief 货币符号位：en/zh/ja/ko 前置，其余（de/fr/ru…）后置（带前导空格）。
[[nodiscard]] inline auto currency_is_prefix(const std::string &lang) -> bool {
    return lang == "en" || lang == "zh" || lang == "ja" || lang == "ko";
}

}  // namespace detail

/**
 * @brief 按 Locale 格式化数字（分组 + 小数点）。
 *
 * @param value           数值。
 * @param loc             区域设置（决定分隔符与小数点）。
 * @param fraction_digits 小数位数；默认 2。传 0 则只输出整数分组（如 "1,234"）。
 * @return 本地化数字串，如 en `1,234.56`、de `1.234,56`、zh `1,234.56`。
 *
 * @note Thread: thread-safe (pure function)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto format_number(double value, const Locale &loc, int fraction_digits = 2) -> std::string {
    const detail::NumberSymbols sym = detail::number_symbols(loc.language);
    const bool negative = value < 0.0;
    const double a = negative ? -value : value;

    double intf = 0.0;
    const double frac = std::modf(a, &intf);
    long long ip = static_cast<long long>(intf);

    std::string frac_str;
    if (fraction_digits > 0) {
        const double scale = std::pow(10.0, fraction_digits);
        long long fpart = static_cast<long long>(std::round(frac * scale));
        if (fpart >= static_cast<long long>(scale)) {  // 四舍五入进位到整数
            ++ip;
            fpart = 0;
        }
        frac_str = std::to_string(fpart);
        while (static_cast<int>(frac_str.size()) < fraction_digits) {
            frac_str = "0" + frac_str;
        }
    }

    std::string ip_str = std::to_string(ip);
    std::string grouped;
    const int len = static_cast<int>(ip_str.size());
    for (int k = 0; k < len; ++k) {
        if (k > 0 && (len - k) % sym.group_size == 0) {
            grouped += sym.group;
        }
        grouped += ip_str[k];
    }

    std::string out;
    if (negative) {
        out += '-';
    }
    out += grouped;
    if (fraction_digits > 0) {
        out += sym.decimal;
        out += frac_str;
    }
    return out;
}

/**
 * @brief 按 Locale 格式化货币金额。
 *
 * @param value           金额数值。
 * @param cur             货币代码（决定符号与默认小数位）。
 * @param loc             区域设置（决定符号位与数字分隔符）。
 * @param fraction_digits 小数位数；默认 -1 表示沿用货币默认（JPY=0、其余=2）。
 * @return 本地化货币串，如 en+USD `$1,234.56`、de+EUR `1.234,56 €`、ja+JPY `¥1,234`。
 *
 * @note Thread: thread-safe (pure function)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto format_currency(double value, Currency cur, const Locale &loc, int fraction_digits = -1)
    -> std::string {
    const detail::CurrencyInfo info = detail::currency_info(cur);
    const int frac = (fraction_digits >= 0) ? fraction_digits : info.default_fraction_digits;
    const std::string num = format_number(value, loc, frac);
    if (detail::currency_is_prefix(loc.language)) {
        return std::string(info.symbol) + num;
    }
    return num + " " + std::string(info.symbol);
}

/**
 * @brief 按 Locale 格式化日期。
 *
 * @param year/month/day 公历年月日。
 * @param loc            区域设置（决定日期模式）。
 * @return 本地化日期串：en `2025-10-25`(ISO)、de `25.10.2025`、zh/ja `2025年10月25日`、fr `25/10/2025`。
 *
 * @note Thread: thread-safe (pure function)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto format_date(int year, int month, int day, const Locale &loc) -> std::string {
    const std::string &lang = loc.language;
    auto p2 = [](int v) -> std::string {
        std::string s = std::to_string(v);
        return s.size() == 1 ? "0" + s : s;
    };
    if (lang == "de") {
        return p2(day) + "." + p2(month) + "." + std::to_string(year);
    }
    if (lang == "fr") {
        return p2(day) + "/" + p2(month) + "/" + std::to_string(year);
    }
    if (lang == "zh" || lang == "ja") {
        return std::to_string(year) + "年" + std::to_string(month) + "月" + std::to_string(day) + "日";
    }
    // en / 默认：ISO 8601
    return std::to_string(year) + "-" + p2(month) + "-" + p2(day);
}

}  // namespace aurora
