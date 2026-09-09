/// 测试类型: unit
/// 目标单元: include/aurora/i18n/localized_string.h
/// 测试说明: LocalizedString 字面构造、tr() 本地化标记、相等语义与经 StringTable 的解析/嵌套参数/回退

#include <string>
#include <string_view>
#include <vector>

#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_localized_string {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）

AURORA_TEST_CASE(implicit_construction_captures_literal_text) {
    // 三种字面形态都落到 text；localize 默认 false、key/args 为空。
    const aurora::LocalizedString from_char = "hi";
    const aurora::LocalizedString from_string = std::string{"hi"};
    const aurora::LocalizedString from_view = std::string_view{"hi"};
    AURORA_TEST_CHECK_EQ(from_char.text, std::string("hi"));
    AURORA_TEST_CHECK_EQ(from_string.text, std::string("hi"));
    AURORA_TEST_CHECK_EQ(from_view.text, std::string("hi"));
    AURORA_TEST_CHECK_FALSE(from_char.localize);
    AURORA_TEST_CHECK(from_char.key.empty());
    AURORA_TEST_CHECK(from_char.args.empty());
}

AURORA_TEST_CASE(default_constructed_is_empty_literal) {
    // 默认构造即空字面量；c_str() 指向空文本。
    const aurora::LocalizedString ls;
    AURORA_TEST_CHECK(ls.text.empty());
    AURORA_TEST_CHECK(ls.key.empty());
    AURORA_TEST_CHECK_FALSE(ls.localize);
    AURORA_TEST_CHECK(ls.args.empty());
    AURORA_TEST_CHECK_STREQ(ls.c_str(), "");
}

AURORA_TEST_CASE(tr_marks_key_and_args_for_lookup) {
    // tr() 生成待查表条目：key/localize/args 就位，text 留空作查表失败回退。
    const auto ls = aurora::LocalizedString::tr("greeting", {aurora::LocalizedString{"Aurora"}});
    AURORA_TEST_CHECK_EQ(ls.key, std::string("greeting"));
    AURORA_TEST_CHECK(ls.localize);
    AURORA_TEST_REQUIRE_THAT(ls.args, m::size_is(1));
    AURORA_TEST_CHECK_EQ(ls.args[0].text, std::string("Aurora"));
    AURORA_TEST_CHECK(ls.text.empty());
}

AURORA_TEST_CASE(resolve_without_table_falls_back_to_text) {
    // 表指针为空：非本地化返回原文本；本地化条目也回退 text（此处为空串）。
    const aurora::Locale loc;
    const aurora::LocalizedString literal{"plain"};
    AURORA_TEST_CHECK_EQ(literal.resolve(nullptr, loc), std::string("plain"));
    const auto missing = aurora::LocalizedString::tr("greeting");
    AURORA_TEST_CHECK(missing.resolve(nullptr, loc).empty());
}

AURORA_TEST_CASE(resolve_formats_via_table_and_supports_nested_args) {
    // 有表且命中：{0} 占位替换；嵌套本地化参数先递归解析再代入。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    table.add(en, "name", "Aurora");
    table.add(en, "greeting", "Hello {0}");
    const auto simple = aurora::LocalizedString::tr("greeting", {aurora::LocalizedString{"Ada"}});
    AURORA_TEST_CHECK_EQ(simple.resolve(&table, en), std::string("Hello Ada"));
    const auto nested = aurora::LocalizedString::tr("greeting", {aurora::LocalizedString::tr("name")});
    AURORA_TEST_CHECK_EQ(nested.resolve(&table, en), std::string("Hello Aurora"));
}

AURORA_TEST_CASE(resolve_missing_key_falls_back_to_text) {
    // 表中无此 key：回退 text（tr 生成的条目 text 为空 → 解析为空串，不抛错）。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    const auto ls = aurora::LocalizedString::tr("absent");
    AURORA_TEST_CHECK(ls.resolve(&table, en).empty());
}

AURORA_TEST_CASE(equality_compares_text_and_key_only) {
    // operator== 只比较 text 与 key（args / localize 不参与）——头文件注明的刻意语义，照实覆盖。
    const aurora::LocalizedString a{"same"};
    const aurora::LocalizedString b{"same"};
    AURORA_TEST_CHECK(a == b);
    AURORA_TEST_CHECK(a != aurora::LocalizedString{"other"});
    AURORA_TEST_CHECK(aurora::LocalizedString::tr("k") == aurora::LocalizedString::tr("k"));
    AURORA_TEST_CHECK(a != aurora::LocalizedString::tr("k"));  // text 与 key 组合不同
}

AURORA_TEST_CASE(c_str_aliases_text_buffer) {
    // c_str() 与 text 共享同一缓冲区。
    const aurora::LocalizedString ls{"aurora"};
    AURORA_TEST_CHECK(ls.c_str() == ls.text.c_str());
    AURORA_TEST_CHECK_STREQ(ls.c_str(), "aurora");
}

}  // namespace aurora::test_cases::utest_localized_string
