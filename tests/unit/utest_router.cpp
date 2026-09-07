/// 测试类型: unit
/// 目标单元: include/aurora/navigation/router.h
/// 测试说明: router 单元测试
///

// 路由表 1:1 测试：登记/查找/构建/未登记降级。
#include <string>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_router {

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_router ===\n");

    Router r;

    // 1) 未登记名称 has 为 false。
    AURORA_TEST_CHECK(!r.has("home"));

    // 2) 登记 home 路由。
    r.register_route("home", []() -> Route { return Route{Node{Text{"home"}}}; });
    AURORA_TEST_CHECK(r.has("home"));

    // 3) 登记第二个路由（settings）。
    r.register_route("settings", []() -> Route { return Route{Node{Column{Node{Text{"s"}}}}}; });
    AURORA_TEST_CHECK(r.has("settings"));

    // 4) 未登记名称仍为 false。
    AURORA_TEST_CHECK(!r.has("other"));

    // 5) build 已登记路由返回有值 optional。
    auto opt = r.build("home");
    AURORA_TEST_CHECK(opt.has_value());

    // 6) 构建出的路由非空（有根节点）。
    AURORA_TEST_CHECK(opt && !opt->empty());

    // 7) 构建出的路由根节点类型为 Text。
    // 前置 CHECK 已记录失败，此处仅取值比对（has_value 断言在上方，失败时用例已判负）。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK(std::string(opt->root().widget().type_name()) == "Text");

    // 8) build 未登记路由返回 nullopt。
    auto none = r.build("other");
    AURORA_TEST_CHECK(!none.has_value());

    // 9) build_root 已登记路由返回非空 Node（根类型 Text）。
    Node home_root = r.build_root("home");
    AURORA_TEST_CHECK(static_cast<bool>(home_root));
    AURORA_TEST_CHECK(std::string(home_root.widget().type_name()) == "Text");

    // 10) build_root 未登记返回空 Node（bool 转换为 false）。
    Node empty_root = r.build_root("other");
    AURORA_TEST_CHECK(!static_cast<bool>(empty_root));
}

}  // namespace aurora::test_cases::utest_router