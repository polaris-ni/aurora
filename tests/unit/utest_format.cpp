/// 测试类型: unit
/// 目标单元: include/aurora/i18n/format.h
/// 测试说明: 数字分组 / 货币符号位与小数位 / 按 Locale 与 DateStyle 的日期模式（满足 Locale
///           完成判据：德语日期、日元/美元货币），以及 CivilDate 值类型与 format_date 的口径对齐。
///           守零依赖自研表。

#include <string>

#include "aurora/i18n/format.h"
#include "aurora/i18n/locale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_format {

using aurora::CivilDate;
using aurora::Currency;
using aurora::DateStyle;
using aurora::format_currency;
using aurora::format_date;
using aurora::format_number;
using aurora::Locale;

AURORA_TEST_CASE(format_number_grouping_and_decimal_separators) {
    // 整数分组：en/zh 逗号分组点小数，de 点分组逗号小数。
    AURORA_TEST_CHECK_EQ(format_number(1234567.0, Locale{.language = "en"}, 0), std::string("1,234,567"));
    AURORA_TEST_CHECK_EQ(format_number(1234567.0, Locale{.language = "de"}, 0), std::string("1.234.567"));
    AURORA_TEST_CHECK_EQ(format_number(1234567.0, Locale{.language = "zh"}, 0), std::string("1,234,567"));
    AURORA_TEST_CHECK_EQ(format_number(1234567.0, Locale{.language = "ja"}, 0), std::string("1,234,567"));

    // 小数两位：分隔符随语言切换。
    AURORA_TEST_CHECK_EQ(format_number(1234.5, Locale{.language = "en"}, 2), std::string("1,234.50"));
    AURORA_TEST_CHECK_EQ(format_number(1234.5, Locale{.language = "de"}, 2), std::string("1.234,50"));
    AURORA_TEST_CHECK_EQ(format_number(1234.5, Locale{.language = "fr"}, 2), std::string("1 234,50"));

    // 负数保留符号。
    AURORA_TEST_CHECK_EQ(format_number(-1234.0, Locale{.language = "en"}, 2), std::string("-1,234.00"));
}

AURORA_TEST_CASE(format_currency_usd_and_jpy) {
    // USD：en 前置 "$"，de 后置 " $"。
    AURORA_TEST_CHECK_EQ(format_currency(1234.56, Currency::USD, Locale{.language = "en"}), std::string("$1,234.56"));
    AURORA_TEST_CHECK_EQ(format_currency(1234.56, Currency::USD, Locale{.language = "de"}), std::string("1.234,56 $"));

    // JPY：0 位小数；en 前置 "¥"（符号取货币自身 ¥，非 $），ja 前置 "¥"。
    AURORA_TEST_CHECK_EQ(format_currency(1234.0, Currency::JPY, Locale{.language = "en"}), std::string("\u00A51,234"));
    AURORA_TEST_CHECK_EQ(format_currency(1234.0, Currency::JPY, Locale{.language = "ja"}), std::string("\u00A51,234"));

    // EUR：de 后置 " €"。
    AURORA_TEST_CHECK_EQ(format_currency(1234.56, Currency::EUR, Locale{.language = "de"}),
                         std::string("1.234,56 \u20AC"));
}

AURORA_TEST_CASE(format_date_by_locale) {
    // 德语日期：dd.MM.yyyy（Locale 完成判据之一）。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "de"}), std::string("25.10.2025"));
    // 英语：ISO 8601。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "en"}), std::string("2025-10-25"));
    // 中文 / 日语：年月日汉字模式。
    // CJK-LITERAL: locale-output - zh/ja 年月日 date units are the feature under test
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "zh"}), std::string("2025年10月25日"));
    AURORA_TEST_CHECK_EQ(format_date(2025, 1, 3, Locale{.language = "ja"}), std::string("2025年1月3日"));
}

AURORA_TEST_CASE(format_date_style_tiers) {
    // medium 是缺省档：显式传入与省略形参逐位一致。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "en"}),
                         format_date(2025, 10, 25, Locale{.language = "en"}, DateStyle::Medium));
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "de"}, DateStyle::Medium),
                         std::string("25.10.2025"));
    // short：en 美式两位年；补零到 2 位。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "en"}, DateStyle::Short),
                         std::string("10/25/25"));
    AURORA_TEST_CHECK_EQ(format_date(2025, 1, 3, Locale{.language = "en"}, DateStyle::Short), std::string("01/03/25"));
    // long：月名表覆盖 en/de/fr。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "en"}, DateStyle::Long),
                         std::string("October 25, 2025"));
    AURORA_TEST_CHECK_EQ(format_date(2025, 3, 1, Locale{.language = "en"}, DateStyle::Long),
                         std::string("March 1, 2025"));
    // long：de/fr 为「日. 月名 年」，月名按各自语言表。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "de"}, DateStyle::Long),
                         std::string("25. Oktober 2025"));
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "fr"}, DateStyle::Long),
                         std::string("25 octobre 2025"));
    // CJK-LITERAL: locale-output - zh/ja 三档同 medium 年月日形态
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "zh"}, DateStyle::Long),
                         std::string("2025年10月25日"));
    // CJK-LITERAL: locale-output - zh short 为斜杠 ISO 形态
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "zh"}, DateStyle::Short),
                         std::string("2025/10/25"));
    // 未覆盖语言（如 it）long 回落 medium 现行口径。
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "it"}, DateStyle::Long),
                         format_date(2025, 10, 25, Locale{.language = "it"}));
    // CivilDate 只是年/月/日的强类型包装：与三分量直调逐位对齐（消息引擎走的就是这条口径）。
    const CivilDate due{.year = 2025, .month = 10, .day = 25};
    AURORA_TEST_CHECK_EQ(format_date(due.year, due.month, due.day, Locale{.language = "en"}, DateStyle::Long),
                         std::string("October 25, 2025"));
}

}  // namespace aurora::test_cases::utest_format
