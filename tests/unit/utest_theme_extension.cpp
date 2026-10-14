/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_extension.h
/// 测试说明: 覆盖类型擦除扩展注册表的值语义——按类型 set/get/remove、同类型覆盖、拷贝即深拷贝
/// （源表变更后副本不受影响）、移动窃取、空表零持有、未注册类型返回 nullptr 且无字符串回落。

#include <utility>
#include <vector>

#include "aurora/theming/theme_extension.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_extension {

namespace {

/// @brief 测试用扩展：一个普通可拷贝值 struct（模拟 `ChartPalette`）。
struct Palette {
    std::vector<int> colors;  ///< 色板值序列。
};

/// @brief 另一扩展类型，验证按类型独立寻址。
struct Metric {
    int gap = 0;  ///< 间距。
};

}  // namespace

AURORA_TEST_CASE(set_then_get_returns_stored_value) {
    ThemeExtensions ext;
    ext.set(Palette{{1, 2, 3}});
    const Palette *p = ext.get<Palette>();
    AURORA_TEST_CHECK(p != nullptr);
    AURORA_TEST_REQUIRE(p != nullptr);
    AURORA_TEST_CHECK_EQ(p->colors.size(), 3U);
    AURORA_TEST_CHECK_EQ(p->colors.front(), 1);
}

AURORA_TEST_CASE(get_unregistered_type_is_null) {
    ThemeExtensions ext;
    ext.set(Palette{{7}});
    AURORA_TEST_CHECK(ext.get<Palette>() != nullptr);
    AURORA_TEST_CHECK(ext.get<Metric>() == nullptr);  // 未注册即 nullptr，无静默回落
}

AURORA_TEST_CASE(set_same_type_replaces) {
    ThemeExtensions ext;
    ext.set(Palette{{1, 2, 3}});
    ext.set(Palette{{9, 9}});  // 同类型整体替换
    const Palette *p = ext.get<Palette>();
    AURORA_TEST_REQUIRE(p != nullptr);
    AURORA_TEST_CHECK_EQ(p->colors.size(), 2U);
    AURORA_TEST_CHECK_EQ(p->colors.front(), 9);
}

AURORA_TEST_CASE(copy_is_deep_value_clone) {
    ThemeExtensions src;
    src.set(Palette{{1, 2, 3}});
    const ThemeExtensions copy = src;  // 深拷贝
    src.set(Palette{{8, 8, 8, 8}});  // 变更源表不影响副本
    const Palette *cp = copy.get<Palette>();
    const Palette *sp = src.get<Palette>();
    AURORA_TEST_REQUIRE(cp != nullptr);
    AURORA_TEST_REQUIRE(sp != nullptr);
    AURORA_TEST_CHECK_EQ(cp->colors.size(), 3U);  // 副本仍持旧值
    AURORA_TEST_CHECK_EQ(sp->colors.size(), 4U);
    AURORA_TEST_CHECK_NE(&cp->colors.front(), &sp->colors.front());  // 独立存储，非别名
}

AURORA_TEST_CASE(move_steals_from_source) {
    ThemeExtensions src;
    src.set(Palette{{4, 5}});
    ThemeExtensions dst = std::move(src);
    AURORA_TEST_CHECK(dst.get<Palette>() != nullptr);
    // moved-from 态检查本身就是本用例的测试点：源表转空是移动语义的契约。
    AURORA_TEST_CHECK(src.get<Palette>() == nullptr);  // NOLINT(bugprone-use-after-move) 有意检查移出后源态
}

AURORA_TEST_CASE(remove_reports_presence) {
    ThemeExtensions ext;
    ext.set(Palette{{1}});
    AURORA_TEST_CHECK(ext.remove<Palette>());  // 存在 → 移除成功
    AURORA_TEST_CHECK(!ext.remove<Palette>());  // 再移除 → false
    AURORA_TEST_CHECK(ext.get<Palette>() == nullptr);
}

AURORA_TEST_CASE(empty_registry_holds_nothing) {
    ThemeExtensions ext;
    AURORA_TEST_CHECK(ext.get<Palette>() == nullptr);
    AURORA_TEST_CHECK(ext.get<Metric>() == nullptr);
}

}  // namespace aurora::test_cases::utest_theme_extension
