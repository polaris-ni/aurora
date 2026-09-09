/// 测试类型: unit
/// 目标单元: include/aurora/i18n/string_table.h
/// 测试说明: StringTable 按 Locale 查表与默认区域回退、占位/复数格式化、花括号配平及 LocalizedString 解析联动

#include <string>
#include <vector>

#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/string_table.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_string_table {

AURORA_TEST_CASE(add_and_lookup_per_locale) {
    // 按 Locale tag 存取：zh 与 zh-CN 是不同条目，互不干扰。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    const aurora::Locale zh_cn{.language = "zh", .region = "CN"};
    table.add(en, "greeting", "Hello");
    table.add(zh_cn, "greeting", "你好");
    AURORA_TEST_REQUIRE(table.lookup("greeting", en).has_value());
    AURORA_TEST_CHECK_EQ(*table.lookup("greeting", en), std::string("Hello"));
    AURORA_TEST_REQUIRE(table.lookup("greeting", zh_cn).has_value());
    AURORA_TEST_CHECK_EQ(*table.lookup("greeting", zh_cn), std::string("你好"));
}

AURORA_TEST_CASE(lookup_falls_back_to_default_locale) {
    // 请求区域缺失 → 回退默认区域；默认区域也没有 → nullopt。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    const aurora::Locale zh{.language = "zh"};
    table.add(en, "hi", "Hello");
    AURORA_TEST_REQUIRE(table.lookup("hi", zh).has_value());
    AURORA_TEST_CHECK_EQ(*table.lookup("hi", zh), std::string("Hello"));  // 默认区域为 en
    table.set_default_locale(zh);
    table.add(zh, "hi", "你好");
    const aurora::Locale fr{.language = "fr"};
    AURORA_TEST_REQUIRE(table.lookup("hi", fr).has_value());
    AURORA_TEST_CHECK_EQ(*table.lookup("hi", fr), std::string("你好"));  // 默认区域改为 zh
    AURORA_TEST_CHECK_FALSE(table.lookup("absent", fr).has_value());
}

AURORA_TEST_CASE(add_overwrites_existing_template) {
    // 同 key 重复 add：后写覆盖先写。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    table.add(en, "k", "old");
    table.add(en, "k", "new");
    AURORA_TEST_REQUIRE(table.lookup("k", en).has_value());
    AURORA_TEST_CHECK_EQ(*table.lookup("k", en), std::string("new"));
}

AURORA_TEST_CASE(format_replaces_positional_placeholders) {
    // {i} 顺序占位替换；越界占位丢弃（不抛错）；无占位原样返回。
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("Hello {0}, you are {1}!", {"Ada", "admin"}),
                         std::string("Hello Ada, you are admin!"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("x{5}y", {"a"}), std::string("xy"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("no args", {}), std::string("no args"));
}

AURORA_TEST_CASE(format_plural_selects_one_or_other) {
    // 简版复数：按 args[idx] 数值选 one/other；分支值可裸写、可含 {0} 占位、可整段花括号包裹。
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("{0, plural, one={0} item other={0} items}", {"1"}),
                         std::string("1 item"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("{0, plural, one={0} item other={0} items}", {"3"}),
                         std::string("3 items"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("{0, plural, one=1 item other=many items}", {"1"}),
                         std::string("1 item"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("{0, plural, one=1 item other=many items}", {"7"}),
                         std::string("many items"));
    AURORA_TEST_CHECK_EQ(aurora::StringTable::format("{0, plural, one={single item} other={many items}}", {"2"}),
                         std::string("many items"));
}

AURORA_TEST_CASE(find_closing_brace_handles_nesting_and_unbalanced) {
    // 配平考虑嵌套花括号；从内层 '{' 起配平到其自身 '}'；无配平返回 npos。
    AURORA_TEST_CHECK_EQ(aurora::StringTable::find_closing_brace("{a{b}c}", 0), std::size_t{6});
    AURORA_TEST_CHECK_EQ(aurora::StringTable::find_closing_brace("{a{b}c}", 2), std::size_t{4});
    AURORA_TEST_CHECK(aurora::StringTable::find_closing_brace("{abc", 0) == std::string::npos);
}

AURORA_TEST_CASE(resolve_localized_and_literal_paths) {
    // resolve：非本地化短路返回原文本；本地化命中走格式化；未命中回退 text。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    table.add(en, "greeting", "Hi {0}");
    AURORA_TEST_CHECK_EQ(table.resolve(aurora::LocalizedString{"plain"}, en), std::string("plain"));
    const auto localized = aurora::LocalizedString::tr("greeting", {aurora::LocalizedString{"Au"}});
    AURORA_TEST_CHECK_EQ(table.resolve(localized, en), std::string("Hi Au"));
    const auto absent = aurora::LocalizedString::tr("absent");
    AURORA_TEST_CHECK(table.resolve(absent, en).empty());  // 回退到空 text
}

AURORA_TEST_CASE(localized_string_resolve_dispatches_to_table) {
    // LocalizedString::resolve 委托 StringTable：嵌套本地化参数递归解析。
    aurora::StringTable table;
    const aurora::Locale en{.language = "en"};
    table.add(en, "name", "Aurora");
    table.add(en, "welcome", "Welcome, {0}!");
    const auto ls = aurora::LocalizedString::tr("welcome", {aurora::LocalizedString::tr("name")});
    AURORA_TEST_CHECK_EQ(ls.resolve(&table, en), std::string("Welcome, Aurora!"));
}

AURORA_TEST_CASE(default_string_table_returns_stable_reference) {
    // 进程级默认表：两次调用返回同一实例（只读冒烟，不写入全局状态）。
    AURORA_TEST_CHECK(&aurora::default_string_table() == &aurora::default_string_table());
}

}  // namespace aurora::test_cases::utest_string_table
