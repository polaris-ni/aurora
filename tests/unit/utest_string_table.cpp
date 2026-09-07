/// 测试类型: unit
/// 目标单元: include/aurora/i18n/string_table.h
/// 测试说明: i18n 字符串表（add/lookup 回退默认区域、resolve 占位与复数格式化、find_closing_brace、进程级默认表）单元测试

#include <optional>
#include <string>

#include "aurora/i18n/string_table.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_string_table {

AURORA_TEST() {
    const Locale en{.language = "en", .region = ""};
    const Locale zh{.language = "zh", .region = ""};

    // ---- 1. add + lookup：命中直接返回模板 ----
    {
        StringTable t;
        t.add(en, "greeting", "Hello {0}");
        t.add(zh, "greeting", "你好 {0}");
        AURORA_TEST_CHECK(t.lookup("greeting", en) == std::optional<std::string>("Hello {0}"));
        AURORA_TEST_CHECK(t.lookup("greeting", zh) == std::optional<std::string>("你好 {0}"));
        AURORA_TEST_CHECK(!t.lookup("missing", en).has_value());
    }

    // ---- 2. lookup：目标区域缺失时回退默认区域 ----
    {
        StringTable t;
        t.add(en, "k", "v-en");
        t.set_default_locale(en);
        const Locale fr{.language = "fr", .region = ""};
        // fr 无 "k"，回退默认 en → 命中
        AURORA_TEST_CHECK(t.lookup("k", fr) == std::optional<std::string>("v-en"));
    }

    // ---- 3. resolve：查表 + {i} 占位替换；非本地化直接返回字面文本 ----
    {
        StringTable t;
        t.add(en, "greeting", "Hello {0}");
        const auto ls = LocalizedString::tr("greeting", {LocalizedString{"Aurora"}});
        AURORA_TEST_CHECK_EQ(t.resolve(ls, en), std::string("Hello Aurora"));

        const LocalizedString raw{std::string("as-is")};
        AURORA_TEST_CHECK_EQ(t.resolve(raw, en), std::string("as-is"));  // localize==false → 直返 text

        // 查表失败：本地化但 key 不存在 → 回退 text（此处为空）
        const auto miss = LocalizedString::tr("nope");
        AURORA_TEST_CHECK(t.resolve(miss, en).empty());
    }

    // ---- 4. format：位置占位、越界跳过 ----
    {
        AURORA_TEST_CHECK_EQ(StringTable::format("{0}+{1}", {"a", "b"}), std::string("a+b"));
        AURORA_TEST_CHECK_EQ(StringTable::format("{0}", std::vector<std::string>{}), std::string(""));
        AURORA_TEST_CHECK_EQ(StringTable::format("no placeholder", {"x"}), std::string("no placeholder"));
    }

    // ---- 5. format：简版复数 one/other 按 args[0] 数值选择 ----
    {
        AURORA_TEST_CHECK_EQ(StringTable::format("{0, plural, one={0} item other={0} items}", {"1"}),
                             std::string("1 item"));
        AURORA_TEST_CHECK_EQ(StringTable::format("{0, plural, one={0} item other={0} items}", {"3"}),
                             std::string("3 items"));
    }

    // ---- 6. find_closing_brace：考虑嵌套花括号的配平定位 ----
    {
        AURORA_TEST_CHECK_EQ(StringTable::find_closing_brace("{a{b}c}x", 0), std::size_t{6});
        AURORA_TEST_CHECK(StringTable::find_closing_brace("no close", 0) == std::string::npos);
    }

    // ---- 7. 进程级默认表可用（单例引用稳定） ----
    {
        auto &d = default_string_table();
        d.add(en, "app.title", "Aurora");
        AURORA_TEST_CHECK(d.lookup("app.title", en) == std::optional<std::string>("Aurora"));
        AURORA_TEST_CHECK(&d == &default_string_table());
    }
}

}  // namespace aurora::test_cases::utest_string_table
