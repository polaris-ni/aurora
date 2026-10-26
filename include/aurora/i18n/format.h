#pragma once

#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/i18n/locale.h"

namespace aurora {

/// @brief 货币代码（覆盖 Locale 完成判据所需：USD / JPY / EUR / CNY 等）。
///
/// 符号与默认小数位取自 CLDR 轻量自研表（守零依赖）；后续可随需求扩展更多代码。
///
/// @note Thread: thread-safe (value type)
/// @note Side-effects: none
/// @note Rebuildable: no
/// @note 豁免 performance-enum-size：公共 API 枚举，仅作 format_currency 的按值形参，不落在任何
/// 结构体/容器字段里；底层类型是公共 API 形态的一部分，本库按语义选型而非体积取向，改窄仅省 3 字节。
/// NOLINTNEXTLINE(performance-enum-size)
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
    char group;  ///< 千位分隔符
    char decimal;  ///< 小数点
    int group_size;  ///< 从右起的分组位数
};

/// @brief 按基础语言子标签取数字分组/小数点符号表（自研轻量表，CLDR 简化版）。
/// @param lang 基础语言子标签（如 "en"/"de"/"fr"）。
/// @return de/ru 点分组、逗号小数；fr 空格分组、逗号小数；其余（en/zh/ja 等）逗号分组、点小数；组宽均为 3。
[[nodiscard]] inline auto number_symbols(const std::string &lang) -> NumberSymbols {
    if (lang == "de" || lang == "ru") {
        return NumberSymbols{.group = '.', .decimal = ',', .group_size = 3};  // 德语 / 俄语：点分组、逗号小数
    }
    if (lang == "fr") {
        // 法语：空格分组（CLDR 真实用窄 NBSP U+202F）、逗号小数
        return NumberSymbols{.group = ' ', .decimal = ',', .group_size = 3};
    }
    return NumberSymbols{.group = ',', .decimal = '.', .group_size = 3};  // en / zh / ja / 其它：逗号分组、点小数
}

/// @brief 货币的符号与默认小数位（CLDR 轻量自研表项）。
struct CurrencyInfo {
    const char *symbol;  ///< 货币符号串（UTF-8，如 "$"/"€"/"¥"/"£"）
    int default_fraction_digits;  ///< 默认小数位数（JPY=0，其余=2）
};

/// @brief 查货币代码对应的符号表项。
/// @param c 货币代码。
/// @return 对应表项；switch 未覆盖的取值（越界枚举）回退 USD（$，2 位小数）。
[[nodiscard]] inline auto currency_info(Currency c) -> CurrencyInfo {
    switch (c) {
        case Currency::USD:
            return CurrencyInfo{.symbol = "$", .default_fraction_digits = 2};
        case Currency::EUR:
            return CurrencyInfo{.symbol = "\u20AC", .default_fraction_digits = 2};  // €
        case Currency::JPY:
            return CurrencyInfo{.symbol = "\u00A5", .default_fraction_digits = 0};  // ¥
        case Currency::CNY:
            return CurrencyInfo{.symbol = "\u00A5", .default_fraction_digits = 2};  // ¥
        case Currency::GBP:
            return CurrencyInfo{.symbol = "\u00A3", .default_fraction_digits = 2};  // £
    }
    return CurrencyInfo{.symbol = "$", .default_fraction_digits = 2};
}

/// @brief 货币符号位：en/zh/ja/ko 前置，其余（de/fr/ru…）后置（带前导空格）。
/// @param lang 基础语言子标签。
/// @return true 表示符号应置于数字之前。
[[nodiscard]] inline auto currency_is_prefix(const std::string &lang) -> bool {
    return lang == "en" || lang == "zh" || lang == "ja" || lang == "ko";
}

}  // namespace detail

/// @brief 按 Locale 格式化数字（分组 + 小数点）。
///
/// @param value           数值。
/// @param loc             区域设置（决定分隔符与小数点）。
/// @param fraction_digits 小数位数；默认 2。传 0 则只输出整数分组（如 "1,234"）。
/// @return 本地化数字串，如 en `1,234.56`、de `1.234,56`、zh `1,234.56`。
///
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto format_number(double value, const Locale &loc, int fraction_digits = 2) -> std::string {
    const detail::NumberSymbols sym = detail::number_symbols(loc.language);
    const bool negative = value < 0.0;
    const double a = negative ? -value : value;

    double intf = 0.0;
    const double frac = std::modf(a, &intf);
    auto ip = static_cast<long long>(intf);

    std::string frac_str;
    if (fraction_digits > 0) {
        const double scale = std::pow(10.0, fraction_digits);
        auto fpart = static_cast<long long>(std::round(frac * scale));
        if (fpart >= static_cast<long long>(scale)) {  // 四舍五入进位到整数
            ++ip;
            fpart = 0;
        }
        frac_str = std::to_string(fpart);
        // 左补零到 fraction_digits 位：size() 无符号、fraction_digits 有符号，用 std::cmp_less 做数学术语比较；
        // 本分支内 fraction_digits > 0，转无符号亦等价。
        while (std::cmp_less(frac_str.size(), fraction_digits)) {
            // 首字符插入用 insert，避免 `"0" + frac_str` 每轮整串复制。
            frac_str.insert(frac_str.begin(), '0');
        }
    }

    const std::string ip_str = std::to_string(ip);
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

/// @brief 按 Locale 格式化货币金额。
///
/// @param value           金额数值。
/// @param cur             货币代码（决定符号与默认小数位）。
/// @param loc             区域设置（决定符号位与数字分隔符）。
/// @param fraction_digits 小数位数；默认 -1 表示沿用货币默认（JPY=0、其余=2）。
/// @return 本地化货币串，如 en+USD `$1,234.56`、de+EUR `1.234,56 €`、ja+JPY `¥1,234`。
///
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
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

/// @brief 日期风格（CLDR 语义的轻量自研子集；medium 为各语言现行输出）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
/// @note 豁免 performance-enum-size：与同文件 `Currency` 同口径——公共 API 枚举、只作 `format_date`
/// 的按值形参，不落任何结构体/容器字段；底层类型属公共 API 形态，本库按语义选型而非体积取向。
/// NOLINTNEXTLINE(performance-enum-size)
enum class DateStyle {
    Short,  ///< 短格式：en `10/25/25`、de `25.10.25`、fr `25/10/2025`、zh/ja `2025/10/25`
    Medium,  ///< 中格式：各语言现行 `format_date` 输出（缺省档）
    Long,  ///< 长格式：en `October 25, 2025`、de `25. Oktober 2025`、fr `25 octobre 2025`、zh/ja 同 medium
};

/// @brief 公历日期值（把 `format_date` 的年/月/日三参数包成强类型，供消息引擎日期占位使用）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct CivilDate {
    int year;  ///< 公历年
    int month;  ///< 公历月（1-12；实现不校验范围，与 format_date 同口径）
    int day;  ///< 公历日（1-31；实现不校验范围）

    /// @brief 相等性比较：三个分量逐项相等（默认实现，形参无名）。
    /// @return year/month/day 全部相等时为 true。
    friend constexpr auto operator==(CivilDate, CivilDate) -> bool = default;
};

/// @brief 按 Locale 与风格格式化日期。
///
/// medium 为缺省档，输出与旧版 `format_date` 逐位一致；short/long 的映射为轻量自研表
/// （月名表仅覆盖 en/de/fr，其余语言按 en 兜底；zh/ja 三档同 medium）。
///
/// @param year  公历年（不补零，原样输出）。
/// @param month 公历月（de/fr 补零到 2 位；zh/ja 不补零；实现不校验范围）。
/// @param day   公历日（de/fr 补零到 2 位；zh/ja 不补零；实现不校验范围）。
/// @param loc            区域设置（决定日期模式）。
/// @param style          风格档；缺省 Medium。
/// @return 本地化日期串：en medium `2025-10-25`、de medium `25.10.2025`、zh/ja medium `2025年10月25日`。
///
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto format_date(int year, int month, int day, const Locale &loc,
                                      DateStyle style = DateStyle::Medium) -> std::string {
    const std::string &lang = loc.language;
    auto p2 = [](int v) -> std::string {
        std::string s = std::to_string(v);
        return s.size() == 1 ? "0" + s : s;
    };
    // 各语言 medium（现行口径）以外的档位；zh/ja 三档同 medium，直接落到末尾的现行分支。
    if (style == DateStyle::Short) {
        if (lang == "de") {
            return p2(day) + "." + p2(month) + "." + p2(year % 100);
        }
        if (lang == "fr") {
            return p2(day) + "/" + p2(month) + "/" + std::to_string(year);
        }
        if (lang == "zh" || lang == "ja") {
            // CJK-LITERAL: locale-output - zh/ja date units are the feature of this branch
            return std::to_string(year) + "/" + p2(month) + "/" + p2(day);
        }
        return p2(month) + "/" + p2(day) + "/" + p2(year % 100);
    }
    if (style == DateStyle::Long) {
        // 月名表按 `std::array<std::string_view, 12>` 存放并以 `.at()` 取值：裸 C 数组的下标在
        // clang-tidy 眼里是「非常量表达式索引」，而这里的下标由调用方传入的 month 归一而来。
        static constexpr std::array<std::string_view, 12> DE_MONTHS = {"Januar",    "Februar", "März",     "April",
                                                                       "Mai",       "Juni",    "Juli",     "August",
                                                                       "September", "Oktober", "November", "Dezember"};
        static constexpr std::array<std::string_view, 12> FR_MONTHS = {"janvier",   "février", "mars",     "avril",
                                                                       "mai",       "juin",    "juillet",  "août",
                                                                       "septembre", "octobre", "novembre", "décembre"};
        static constexpr std::array<std::string_view, 12> EN_MONTHS = {"January",   "February", "March",    "April",
                                                                       "May",       "June",     "July",     "August",
                                                                       "September", "October",  "November", "December"};
        const auto index = static_cast<std::size_t>((month - 1 + 12) % 12);
        if (lang == "de") {
            return std::to_string(day) + ". " + std::string(DE_MONTHS.at(index)) + " " + std::to_string(year);
        }
        if (lang == "fr") {
            return std::to_string(day) + " " + std::string(FR_MONTHS.at(index)) + " " + std::to_string(year);
        }
        if (lang == "en") {
            return std::string(EN_MONTHS.at(index)) + " " + std::to_string(day) + ", " + std::to_string(year);
        }
        // 其余语言三档同 medium，落到下方现行分支。
    }
    if (lang == "de") {
        return p2(day) + "." + p2(month) + "." + std::to_string(year);
    }
    if (lang == "fr") {
        return p2(day) + "/" + p2(month) + "/" + std::to_string(year);
    }
    if (lang == "zh" || lang == "ja") {
        // CJK-LITERAL: locale-output - zh/ja 年/月/日 date units are the feature of this branch
        return std::to_string(year) + "年" + std::to_string(month) + "月" + std::to_string(day) + "日";
    }
    // en / 默认：ISO 8601
    return std::to_string(year) + "-" + p2(month) + "-" + p2(day);
}

}  // namespace aurora
