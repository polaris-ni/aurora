// i18n_test.cpp — 覆盖 i18n（StringTable 增删查/格式化/复数、LocalizedString 解析）。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
// ── API 覆盖映射 ─────────────────────────────
// i18n/locale.h(Locale{language,region,tag()})、i18n/string_table.h(StringTable)、i18n/localized_string.h(LocalizedString)。

#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::default_string_table;
using aurora::Locale;
using aurora::LocalizedString;
using aurora::StringTable;

static void test_string_table() {
    StringTable tbl;
    tbl.add(Locale{ .language = "en" }, "greet", "Hello {0}");

    auto looked = tbl.lookup("greet", Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(looked.has_value() && *looked == "Hello {0}", "StringTable: lookup raw template");

    LocalizedString ls = LocalizedString::tr("greet", { LocalizedString{ "World" } });
    std::string r = tbl.resolve(ls, Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(r == "Hello World", "StringTable: resolve formats {0}");

    // 默认区域回退
    tbl.add(Locale{ .language = "en" }, "only_en", "EN only");
    auto fb = tbl.lookup("only_en", Locale{ .language = "zh" });
    AURORA_TEST_CHECK_MSG(fb.has_value() && *fb == "EN only", "StringTable: falls back to default locale");

    // 复数
    tbl.add(Locale{ .language = "en" }, "count", "{0, plural, one={one item} other={other items}}");
    LocalizedString ls1 = LocalizedString::tr("count", { LocalizedString{ "1" } });
    LocalizedString ls3 = LocalizedString::tr("count", { LocalizedString{ "3" } });
    AURORA_TEST_CHECK_MSG(tbl.resolve(ls1, Locale{ "en" }) == "one item", "StringTable: plural one");
    AURORA_TEST_CHECK_MSG(tbl.resolve(ls3, Locale{ "en" }) == "other items", "StringTable: plural other");
}

static void test_default_table() {
    auto &def = default_string_table();
    def.add(Locale{ .language = "en" }, "app_key", "Value {0}");
    const LocalizedString ls = LocalizedString::tr("app_key", { LocalizedString{ "X" } });
    const std::string r = def.resolve(ls, Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(r == "Value X", "default_string_table: add + resolve");
}

static void test_localized_string() {
    // 非本地化回退到字面量
    const LocalizedString plain{ "Hi" };
    AURORA_TEST_CHECK_MSG(plain.resolve(nullptr, Locale{ "en" }) == "Hi",
                          "LocalizedString: non-localized returns text");
    AURORA_TEST_CHECK_MSG(plain.resolve(&default_string_table(), Locale{ "en" }) == "Hi",
                          "LocalizedString: fallback with table");

    // 递归参数解析
    StringTable tbl;
    tbl.add(Locale{ .language = "en" }, "greet", "Hello {0}");
    const LocalizedString nested =
        LocalizedString::tr("greet", { LocalizedString::tr("greet", { LocalizedString{ "Aurora" } }) });
    const std::string r = tbl.resolve(nested, Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(r == "Hello Hello Aurora", "LocalizedString: recursive args resolve");
}

// 运行时解析：default_string_table + 占位 + 复数（从 components_test 归并，使 i18n 测试自洽于本子系统）。
static void test_i18n_runtime() {
    auto &t = default_string_table();
    t.add(Locale{ .language = "en" }, "greeting", "Hello {0}");
    t.add(Locale{ .language = "zh" }, "greeting", "你好 {0}");
    t.add(Locale{ .language = "en" }, "items", "{0, plural, one={0} item other={0} items}");

    const std::string r1 =
        t.resolve(LocalizedString::tr("greeting", { LocalizedString{ "Aurora" } }), Locale{ .language = "zh" });
    AURORA_TEST_CHECK_MSG(r1 == "你好 Aurora", "i18n zh greeting");

    const std::string r2 =
        t.resolve(LocalizedString::tr("items", { LocalizedString{ "1" } }), Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(r2 == "1 item", "i18n plural one");

    const std::string r3 =
        t.resolve(LocalizedString::tr("items", { LocalizedString{ "3" } }), Locale{ .language = "en" });
    AURORA_TEST_CHECK_MSG(r3 == "3 items", "i18n plural other");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== i18n_test ===\n");
    test_string_table();
    test_default_table();
    test_localized_string();
    test_i18n_runtime();
}
