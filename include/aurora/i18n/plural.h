#pragma once

#include <cmath>
#include <string>

#include "aurora/i18n/locale.h"

namespace aurora {

/**
 * @brief CLDR 复数类别（cardinal，基数）。
 *
 * 六类与 CLDR（Unicode 通用 locale 数据仓库）定义一致：每个语言至少含 `Other`，
 * 其余类别仅在该语言确有不同措辞时才出现（如阿拉伯语六类齐全、英语仅 one/other）。
 * 类别名只是「一组数字的标签」，不携带语义——具体数字映射到哪一类由 `plural_category` 决定。
 *
 * @note Thread: thread-safe (pure function)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
enum class PluralCategory {
    Zero,   ///< CLDR zero：如阿拉伯语 0
    One,    ///< CLDR one：如英语 1、法语 0/1、俄语 1/21/101（非 11）
    Two,    ///< CLDR two：如阿拉伯语 2（双数）
    Few,    ///< CLDR few：如俄语 2-4（非 12-14）、阿拉伯语 3-10
    Many,   ///< CLDR many：如俄语 0/5-20、阿拉伯语 11-99
    Other,  ///< CLDR other：兜底类别，所有语言必备
};

namespace detail {

/// @brief 英语 / 德语 / 及其它未知语言的回退规则：仅 one(=1 整数) 与 other。
[[nodiscard]] inline auto plural_category_en(double n) -> PluralCategory {
    const double a = n < 0.0 ? -n : n;
    const long long i = static_cast<long long>(std::trunc(a));
    const bool has_fraction = (a != static_cast<double>(i));
    if (!has_fraction && i == 1) {
        return PluralCategory::One;
    }
    return PluralCategory::Other;
}

/// @brief 中文 / 日语等：仅 other 一类（名词本身不随数变化）。
[[nodiscard]] inline auto plural_category_other_only(double /*n*/) -> PluralCategory {
    return PluralCategory::Other;
}

/// @brief 法语（CLDR fr）：one = 整数 0/1；many = 紧凑百万整数倍（i % 1e6 == 0 且 v=0）；
///       其余为 other。注：科学记数法大指数情形（e != 0..5）本自研实现不覆盖，
///       调用方通常传入归一化后的普通计数值，不影响日常 UI 计数。
[[nodiscard]] inline auto plural_category_fr(double n) -> PluralCategory {
    const double a = n < 0.0 ? -n : n;
    const long long i = static_cast<long long>(std::trunc(a));
    if (i == 0 || i == 1) {
        return PluralCategory::One;
    }
    if (i != 0 && i % 1000000LL == 0) {
        return PluralCategory::Many;
    }
    return PluralCategory::Other;
}

/// @brief 俄语及同族（ru/uk/be/sr/hr/bs/sh）：CLDR 规则表驱动。
[[nodiscard]] inline auto plural_category_ru(double n) -> PluralCategory {
    const double a = n < 0.0 ? -n : n;
    const long long i = static_cast<long long>(std::trunc(a));
    const bool has_fraction = (a != static_cast<double>(i));
    if (!has_fraction) {
        const long long mod10 = i % 10;
        const long long mod100 = ((i % 100) + 100) % 100;  // 取非负模
        if (mod10 == 1 && mod100 != 11) {
            return PluralCategory::One;  // 1, 21, 31, 101 …（非 11）
        }
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) {
            return PluralCategory::Few;  // 2-4, 22-24, 102 …
        }
        return PluralCategory::Many;  // 0, 5-9, 11-14, 20, 25-29 …
    }
    return PluralCategory::Other;  // 分数
}

/// @brief 阿拉伯语（CLDR ar）：六类齐全，全部基于 n 的整数关系。
[[nodiscard]] inline auto plural_category_ar(double n) -> PluralCategory {
    const double a = n < 0.0 ? -n : n;
    if (a == 0.0) {
        return PluralCategory::Zero;
    }
    if (a == 1.0) {
        return PluralCategory::One;
    }
    if (a == 2.0) {
        return PluralCategory::Two;
    }
    const double m100 = std::fmod(a, 100.0);
    if (m100 >= 3.0 && m100 <= 10.0) {
        return PluralCategory::Few;  // 3-10, 103-110 …
    }
    if (m100 >= 11.0 && m100 <= 99.0) {
        return PluralCategory::Many;  // 11-99, 111-199 …
    }
    return PluralCategory::Other;  // 小数、100-102、200-202 …
}

}  // namespace detail

/**
 * @brief 按 CLDR 基数规则把数值 n 映射到复数类别，规则表由 `loc.language` 选择。
 *
 * 未知语言回退英语规则（`one`=`n==1`、`other`=其余），与既有 `num==1` 二元行为向后兼容。
 *
 * @param n   计数值（非负计数语义；负值取绝对值后判定，避免符号干扰分类）。
 * @param loc 区域设置；仅使用 `loc.language`（基础语言子标签，如 "en"/"ar"/"zh"）。
 * @return 对应的 CLDR 复数类别。
 *
 * @note Thread: thread-safe (pure function)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
[[nodiscard]] inline auto plural_category(double n, const Locale &loc) -> PluralCategory {
    const std::string &lang = loc.language;
    if (lang == "ar") {
        return detail::plural_category_ar(n);
    }
    if (lang == "ru" || lang == "uk" || lang == "be" || lang == "sr" || lang == "hr" ||
        lang == "bs" || lang == "sh") {
        return detail::plural_category_ru(n);
    }
    if (lang == "fr") {
        return detail::plural_category_fr(n);
    }
    if (lang == "zh" || lang == "ja" || lang == "ko" || lang == "th" || lang == "vi") {
        return detail::plural_category_other_only(n);
    }
    // en / de / 及其它未知语言：one(=1 整数) / other
    return detail::plural_category_en(n);
}

}  // namespace aurora
