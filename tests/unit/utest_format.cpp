/// 测试类型: unit
/// 目标单元: include/aurora/i18n/format.h
/// 测试说明: 数字分组 / 货币符号位与小数位 / 按 Locale 的日期模式（满足 Locale 完成判据：
///           德语日期、日元/美元货币）。守零依赖自研表。

#include <string>

#include "aurora/i18n/format.h"
#include "aurora/i18n/locale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_format {

using aurora::Currency;
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
    AURORA_TEST_CHECK_EQ(format_date(2025, 10, 25, Locale{.language = "zh"}), std::string("2025年10月25日"));
    AURORA_TEST_CHECK_EQ(format_date(2025, 1, 3, Locale{.language = "ja"}), std::string("2025年1月3日"));
}

}  // namespace aurora::test_cases::utest_format
