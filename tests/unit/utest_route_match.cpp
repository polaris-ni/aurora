/// 测试类型: unit
/// 目标单元: src/aurora/inspector/route_match.h
/// 测试说明: 覆盖 Inspector 路由匹配器的三条不变量——精确项整体优先于前缀项（与登记次序
/// 无关）、前缀项之间按登记次序先登记者先命中、无命中回 nullptr。合成表刻意让精确项与前缀族
/// 重叠：现网生产表没有重叠，若只用生产表断言，「精确优先」会退化成一条空转断言（两趟查找
/// 顺序颠倒也不改变任何分派）。本文件用重叠合成表把该次序钉死；开发期把 `match_route` 的
/// 两趟查找对调后，`route_match_prefers_exact_over_prefix` 必须转红（已实测，随后复原）。
/// 纯字符串比较，无共享状态、无线程与 I/O 依赖。

#include <cstddef>
#include <span>
#include <string_view>

#include "aurora/inspector/route_match.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_route_match {

namespace {

using aurora::inspector::detail::match_route;
using aurora::inspector::detail::RouteKind;

/// @brief 合成条目：只暴露匹配器要求的两枚字段，外加一个标签供断言命中哪一条。
struct Entry {
    std::string_view pattern;
    RouteKind kind;
    std::string_view label;
};

/// @brief 便捷封装：数组转 span（`match_route` 的模板形参是 span，数组不能直接推导）。
template <std::size_t N>
auto lookup(const Entry (&table)[N], std::string_view route) -> const Entry * {
    return match_route(std::span<const Entry>(table), route);
}

}  // namespace

AURORA_TEST_CASE(route_match_prefers_exact_over_prefix) {
    // 前缀族先登记、精确项后登记：若匹配器按「登记次序一趟扫」，精确项会被前缀族抢走；
    // 正确实现走两趟（先全体精确项、再前缀族），故这里必须命中精确项。
    const Entry table[] = {
        {.pattern = "/api/widget/", .kind = RouteKind::Prefix, .label = "prefix"},
        {.pattern = "/api/widget/fixed", .kind = RouteKind::Exact, .label = "exact"},
    };

    const Entry *hit = lookup(table, "/api/widget/fixed");
    AURORA_TEST_REQUIRE_TRUE(hit != nullptr);
    AURORA_TEST_CHECK_STREQ(hit->label, "exact");
    AURORA_TEST_CHECK_EQ(hit->kind, RouteKind::Exact);
}

AURORA_TEST_CASE(route_match_falls_back_to_prefix_when_exact_misses) {
    const Entry table[] = {
        {.pattern = "/api/widget/", .kind = RouteKind::Prefix, .label = "prefix"},
        {.pattern = "/api/widget/fixed", .kind = RouteKind::Exact, .label = "exact"},
    };

    // 精确项不相等时回落到前缀族（同一张表，验证两趟查找的第二趟真的会跑）。
    const Entry *hit = lookup(table, "/api/widget/0/content");
    AURORA_TEST_REQUIRE_TRUE(hit != nullptr);
    AURORA_TEST_CHECK_STREQ(hit->label, "prefix");
    AURORA_TEST_CHECK_EQ(hit->kind, RouteKind::Prefix);

    // 前缀项要求整段前缀相等，而不是「只要共享开头」：`/api/widgetX` 不命中 `/api/widget/`。
    AURORA_TEST_CHECK_TRUE(lookup(table, "/api/widgetX") == nullptr);
}

AURORA_TEST_CASE(route_match_prefix_keeps_registration_order) {
    // 两个重叠前缀族：先登记者先命中。这是「更具体的前缀族可以排在更宽的前缀族之前」的
    // 表达方式——前缀项不做最长匹配。
    const Entry table[] = {
        {.pattern = "/api/widget/", .kind = RouteKind::Prefix, .label = "widget"},
        {.pattern = "/api/", .kind = RouteKind::Prefix, .label = "wide"},
    };

    const Entry *hit = lookup(table, "/api/widget/0");
    AURORA_TEST_REQUIRE_TRUE(hit != nullptr);
    AURORA_TEST_CHECK_STREQ(hit->label, "widget");

    // 把重叠项反过来登记，命中随之翻转——证明次序确实由登记顺序承载。
    const Entry reversed[] = {
        {.pattern = "/api/", .kind = RouteKind::Prefix, .label = "wide"},
        {.pattern = "/api/widget/", .kind = RouteKind::Prefix, .label = "widget"},
    };
    const Entry *wide = lookup(reversed, "/api/widget/0");
    AURORA_TEST_REQUIRE_TRUE(wide != nullptr);
    AURORA_TEST_CHECK_STREQ(wide->label, "wide");
}

AURORA_TEST_CASE(route_match_returns_null_when_absent) {
    const Entry table[] = {
        {.pattern = "/api/tree", .kind = RouteKind::Exact, .label = "tree"},
        {.pattern = "/api/widget/", .kind = RouteKind::Prefix, .label = "widget"},
    };

    AURORA_TEST_CHECK_TRUE(lookup(table, "/api/unknown") == nullptr);
    AURORA_TEST_CHECK_TRUE(lookup(table, "/api/treex") == nullptr);
    AURORA_TEST_CHECK_TRUE(lookup(table, "") == nullptr);
}

AURORA_TEST_CASE(route_match_empty_table) {
    // 生产表恒非空，但模板不应假设这一点：零长度 span 必须平静地回 nullptr。
    AURORA_TEST_CHECK_TRUE(match_route(std::span<const Entry>(), "/api/tree") == nullptr);
}

}  // namespace aurora::test_cases::utest_route_match
