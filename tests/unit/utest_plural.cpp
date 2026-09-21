/// 测试类型: unit
/// 目标单元: include/aurora/i18n/plural.h
/// 测试说明: CLDR 六类复数分类器——各语言代表值映射到正确类别；向后兼容 num==1→One、其余→Other；
///           未知语言回退 en 规则（规则依据 Unicode CLDR plurals.json 实测）

#include <string>

#include "aurora/i18n/locale.h"
#include "aurora/i18n/plural.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_plural {

using aurora::Locale;
using aurora::plural_category;
using aurora::PluralCategory;

// 把类别按整数比较，规避测试框架对 enum class 的字面值打印差异。
static auto c(const std::string &lang, double n) -> int {
    return static_cast<int>(plural_category(n, Locale{.language = lang}));
}
static constexpr int AURORA_ZERO = static_cast<int>(PluralCategory::Zero);
static constexpr int AURORA_ONE = static_cast<int>(PluralCategory::One);
static constexpr int AURORA_TWO = static_cast<int>(PluralCategory::Two);
static constexpr int AURORA_FEW = static_cast<int>(PluralCategory::Few);
static constexpr int AURORA_MANY = static_cast<int>(PluralCategory::Many);
static constexpr int AURORA_OTHER = static_cast<int>(PluralCategory::Other);

AURORA_TEST_CASE(english_one_and_other) {
    // 英语：仅 one(=1 整数) / other；向后兼容 num==1→One、其余→Other。
    AURORA_TEST_CHECK_EQ(c("en", 0.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("en", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("en", 2.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("en", 5.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("en", 100.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("en", 1.5), AURORA_OTHER);  // 带分数 → other（旧式 to_int 会错判为 1，新版修正）
}

AURORA_TEST_CASE(german_one_and_other) {
    // 德语规则同英语（i=1 且 v=0 → one）。
    AURORA_TEST_CHECK_EQ(c("de", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("de", 0.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("de", 42.0), AURORA_OTHER);
}

AURORA_TEST_CASE(chinese_japanese_other_only) {
    // 中文 / 日语：名词不随数变化，恒为 other。
    AURORA_TEST_CHECK_EQ(c("zh", 0.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("zh", 1.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("zh", 2.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("zh", 100.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("ja", 1.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("ja", 1000000.0), AURORA_OTHER);
}

AURORA_TEST_CASE(french_one_many_other) {
    // 法语 CLDR：one = 整数 0/1；many = 百万整数倍（紧凑计数）；其余 other。
    AURORA_TEST_CHECK_EQ(c("fr", 0.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("fr", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("fr", 0.5), AURORA_ONE);  // i=0 → one
    AURORA_TEST_CHECK_EQ(c("fr", 1.5), AURORA_ONE);  // i=1 → one
    AURORA_TEST_CHECK_EQ(c("fr", 2.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("fr", 17.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("fr", 1000000.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("fr", 2000000.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("fr", 1000001.0), AURORA_OTHER);  // 非百万整数倍
}

AURORA_TEST_CASE(russian_one_few_many_other) {
    // 俄语 CLDR：one/few/many/other；21→one、22→few、25→many、11→many。
    AURORA_TEST_CHECK_EQ(c("ru", 0.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("ru", 2.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ru", 3.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ru", 4.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ru", 5.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 9.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 10.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 11.0), AURORA_MANY);  // 11-14 → many
    AURORA_TEST_CHECK_EQ(c("ru", 12.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 14.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 21.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("ru", 22.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ru", 24.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ru", 25.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 100.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ru", 101.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("ru", 1.5), AURORA_OTHER);  // 分数 → other
}

AURORA_TEST_CASE(arabic_six_categories) {
    // 阿拉伯语 CLDR：六类齐全。
    AURORA_TEST_CHECK_EQ(c("ar", 0.0), AURORA_ZERO);
    AURORA_TEST_CHECK_EQ(c("ar", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("ar", 2.0), AURORA_TWO);
    AURORA_TEST_CHECK_EQ(c("ar", 3.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ar", 10.0), AURORA_FEW);
    AURORA_TEST_CHECK_EQ(c("ar", 11.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ar", 99.0), AURORA_MANY);
    AURORA_TEST_CHECK_EQ(c("ar", 100.0), AURORA_OTHER);  // 100-102 → other
    AURORA_TEST_CHECK_EQ(c("ar", 101.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("ar", 102.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("ar", 103.0), AURORA_FEW);  // 103-110 → few
    AURORA_TEST_CHECK_EQ(c("ar", 111.0), AURORA_MANY);  // 111-199 → many
    AURORA_TEST_CHECK_EQ(c("ar", 0.5), AURORA_OTHER);  // 小数 → other
}

AURORA_TEST_CASE(unknown_language_falls_back_to_english) {
    // 未知语言回退英语规则：1→One、其余→Other。
    AURORA_TEST_CHECK_EQ(c("xx", 1.0), AURORA_ONE);
    AURORA_TEST_CHECK_EQ(c("xx", 5.0), AURORA_OTHER);
    AURORA_TEST_CHECK_EQ(c("nl", 1.0), AURORA_ONE);  // 荷语同英语两分类
    AURORA_TEST_CHECK_EQ(c("nl", 3.0), AURORA_OTHER);
}

}  // namespace aurora::test_cases::utest_plural
