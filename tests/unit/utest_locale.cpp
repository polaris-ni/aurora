/// 测试类型: unit
/// 目标单元: include/aurora/i18n/locale.h
/// 测试说明: Locale 默认值、language-region 标签实时合成与纯值语义拷贝

#include <string>

#include "aurora/i18n/locale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_locale {

AURORA_TEST_CASE(default_locale_is_english_without_region) {
    // 默认构造：language 为 "en"、region 为空。
    const aurora::Locale loc;
    AURORA_TEST_CHECK_EQ(loc.language, std::string("en"));
    AURORA_TEST_CHECK(loc.region.empty());
}

AURORA_TEST_CASE(tag_joins_language_and_region) {
    // 有 region 时 tag 以连字符拼接。
    const aurora::Locale loc{.language = "zh", .region = "CN"};
    AURORA_TEST_CHECK_EQ(loc.tag(), std::string("zh-CN"));
    const aurora::Locale pt_br{.language = "pt", .region = "BR"};
    AURORA_TEST_CHECK_EQ(pt_br.tag(), std::string("pt-BR"));
}

AURORA_TEST_CASE(tag_omits_empty_region) {
    // region 为空时 tag 只含 language（查表主键即该形态）。
    const aurora::Locale loc{.language = "fr"};
    AURORA_TEST_CHECK_EQ(loc.tag(), std::string("fr"));
    const aurora::Locale explicit_empty{.language = "de", .region = ""};
    AURORA_TEST_CHECK_EQ(explicit_empty.tag(), std::string("de"));
}

AURORA_TEST_CASE(region_change_retags_immediately) {
    // tag 由当前字段实时合成，修改字段即改变标签。
    aurora::Locale loc{.language = "en"};
    AURORA_TEST_CHECK_EQ(loc.tag(), std::string("en"));
    loc.region = "US";
    AURORA_TEST_CHECK_EQ(loc.tag(), std::string("en-US"));
}

AURORA_TEST_CASE(value_semantics_copy_is_independent) {
    // 纯值类型：拷贝独立，改动副本不影响原值。
    const aurora::Locale base{.language = "pt", .region = "BR"};
    const aurora::Locale& copy = base;
    AURORA_TEST_CHECK_EQ(copy.language, std::string("pt"));
    AURORA_TEST_CHECK_EQ(copy.region, std::string("BR"));
    AURORA_TEST_CHECK_EQ(copy.tag(), base.tag());
    aurora::Locale mutated = base;
    mutated.region = "PT";
    AURORA_TEST_CHECK_EQ(base.tag(), std::string("pt-BR"));  // 原值不受影响
    AURORA_TEST_CHECK_EQ(mutated.tag(), std::string("pt-PT"));
}

}  // namespace aurora::test_cases::utest_locale
