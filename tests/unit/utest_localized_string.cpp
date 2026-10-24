/// 测试类型: unit
/// 目标单元: include/aurora/i18n/localized_string.h
/// 测试说明: LocalizedString v2 值模型——字面构造、tr(MessageId, 具名参数)、
///           相等性口径（text+id）与 resolve 回退链（无目录 / 无效 Id → 字面）

#include <string>

#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/message_catalog.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_localized_string {

AURORA_TEST_CASE(literal_constructions_are_non_localized) {
    // 三种字面构造：localize 恒为 false，resolve 无目录也原样返回。
    const aurora::LocalizedString from_cstr{"Hi"};
    AURORA_TEST_CHECK(!from_cstr.localize);
    AURORA_TEST_CHECK_EQ(from_cstr.resolve(nullptr, aurora::Locale{}), std::string("Hi"));
    const aurora::LocalizedString from_std{std::string("Hey")};
    AURORA_TEST_CHECK_EQ(from_std.resolve(nullptr, aurora::Locale{}), std::string("Hey"));
    const aurora::LocalizedString from_view{std::string_view("Yo")};
    AURORA_TEST_CHECK_EQ(from_view.resolve(nullptr, aurora::Locale{}), std::string("Yo"));
}

AURORA_TEST_CASE(tr_carries_id_and_named_args) {
    // tr：localize 为 true、Id 原样、具名参数保序。
    const auto ls =
        aurora::LocalizedString::tr(aurora::MessageId{7}, {{"name", std::string{"Ada"}}, {"count", std::int64_t{3}}});
    AURORA_TEST_CHECK(ls.localize);
    AURORA_TEST_CHECK_EQ(ls.id, aurora::MessageId{7});
    AURORA_TEST_REQUIRE(ls.args.size() == 2);
    AURORA_TEST_CHECK_EQ(ls.args[0].name, std::string("name"));
    AURORA_TEST_CHECK_EQ(std::get<std::int64_t>(ls.args[1].value), std::int64_t{3});
}

AURORA_TEST_CASE(resolve_without_catalog_falls_back_to_text) {
    // 无目录（nullptr）→ 字面回退：text 是 tr 失败时的显示兜底。
    auto ls = aurora::LocalizedString::tr(aurora::MessageId{7});
    ls.text = "fallback";
    AURORA_TEST_CHECK_EQ(ls.resolve(nullptr, aurora::Locale{}), std::string("fallback"));
}

AURORA_TEST_CASE(resolve_with_invalid_id_falls_back_to_text) {
    // localize 为 true 但 Id 无效 → 走字面（不经目录）。
    auto ls = aurora::LocalizedString::tr(aurora::AURORA_INVALID_MESSAGE_ID);
    ls.text = "orphan";
    aurora::MessageCatalog catalog;
    AURORA_TEST_CHECK_EQ(ls.resolve(&catalog, aurora::Locale{}), std::string("orphan"));
}

AURORA_TEST_CASE(equality_compares_text_and_id_only) {
    // == 口径：text 与 id 逐项相等；args/localize 不参与（与旧版 text+key 口径对位）。
    const auto a = aurora::LocalizedString::tr(aurora::MessageId{1});
    const auto b = aurora::LocalizedString::tr(aurora::MessageId{1});
    AURORA_TEST_CHECK(a == b);
    const auto with_args = aurora::LocalizedString::tr(aurora::MessageId{1}, {{"n", std::int64_t{1}}});
    AURORA_TEST_CHECK(a == with_args);  // args 不参与比较
    const auto other = aurora::LocalizedString::tr(aurora::MessageId{2});
    AURORA_TEST_CHECK(a != other);
    const aurora::LocalizedString literal{"same"};
    const aurora::LocalizedString literal2{"same"};
    AURORA_TEST_CHECK(literal == literal2);
    AURORA_TEST_CHECK(literal != a);
}

AURORA_TEST_CASE(message_arg_value_variants) {
    // MessageArg 值形态：文本 / 整数 / 浮点 / 日期 各走各的 variant 分支。
    const aurora::MessageArg text{"k", std::string{"v"}};
    AURORA_TEST_CHECK(std::holds_alternative<std::string>(text.value));
    const aurora::MessageArg integer{"k", std::int64_t{5}};
    AURORA_TEST_CHECK(std::holds_alternative<std::int64_t>(integer.value));
    const aurora::MessageArg real{"k", 1.5};
    AURORA_TEST_CHECK(std::holds_alternative<double>(real.value));
    const aurora::MessageArg date{"k", aurora::CivilDate{.year = 2025, .month = 10, .day = 25}};
    AURORA_TEST_CHECK(std::holds_alternative<aurora::CivilDate>(date.value));
    AURORA_TEST_CHECK_EQ(std::get<aurora::CivilDate>(date.value),
                         aurora::CivilDate{.year = 2025, .month = 10, .day = 25});
}

}  // namespace aurora::test_cases::utest_localized_string
