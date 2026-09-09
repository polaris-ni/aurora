/// 测试类型: unit
/// 目标单元: include/aurora/navigation/navigator.h
/// 测试说明: 覆盖 Navigator 页面栈语义——push/pop/replace/popToRoot、栈空 pop 防御、
/// 当前页与栈深查询、onRouteChanged 回调计数、栈深上限守卫、deep linking
/// （path 导出 / restore 重建与守卫 / open_uri 切分与注册表缺段跳过）

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aurora/navigation/navigator.h"
#include "aurora/navigation/route.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_navigator {

namespace {

/// 纯色填充探针叶控件（测试仅需一个真实 widget 实例作路由根）。
class SolidBox final : public LeafWidget {
  public:
    SolidBox() = default;

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

/// 按名称构建路由（根 SolidBox + 同名 Route），供 restore/open_uri 使用。
auto named_route(const std::string &name) -> Route {
    return Route{Node{SolidBox{}}, name};
}

}  // namespace

AURORA_TEST_CASE(navigator_initial_route_and_depth) {
    Navigator nav{named_route("home")};  // current_root() 非 const，不能以 const 对象调用

    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_FALSE(nav.can_pop());
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"home"});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(nav.current_root()));
    AURORA_TEST_REQUIRE_EQ(nav.stack().size(), 1U);
    AURORA_TEST_CHECK_EQ(nav.stack()[0].name(), std::string{"home"});
    AURORA_TEST_CHECK_EQ(nav.max_depth(), AURORA_DEFAULT_MAX_NAV_DEPTH);
}

AURORA_TEST_CASE(navigator_push_and_can_pop) {
    Navigator nav{named_route("home")};
    nav.push(named_route("detail"));

    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_TRUE(nav.can_pop());
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"detail"});

    nav.push(named_route("settings"));
    AURORA_TEST_CHECK_EQ(nav.depth(), 3U);
    AURORA_TEST_CHECK_EQ(nav.stack()[1].name(), std::string{"detail"});
}

AURORA_TEST_CASE(navigator_pop_semantics) {
    Navigator nav{named_route("home")};

    // 仅剩根路由：pop 拒绝且栈不变。
    AURORA_TEST_CHECK_FALSE(nav.pop());
    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"home"});

    nav.push(named_route("detail"));
    AURORA_TEST_CHECK_TRUE(nav.pop());
    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"home"});
    AURORA_TEST_CHECK_FALSE(nav.can_pop());
}

AURORA_TEST_CASE(navigator_push_replacement_in_place) {
    // 空栈上 replace 退化为压入。
    Navigator fresh;
    fresh.push_replacement(named_route("home"));
    AURORA_TEST_CHECK_EQ(fresh.depth(), 1U);
    AURORA_TEST_CHECK_EQ(fresh.current().name(), std::string{"home"});

    // 非空栈：原地替换栈顶，深度不变。
    Navigator nav{named_route("home")};
    nav.push(named_route("detail"));
    nav.push_replacement(named_route("edited"));
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"edited"});
    AURORA_TEST_CHECK_EQ(nav.stack()[0].name(), std::string{"home"});
}

AURORA_TEST_CASE(navigator_pop_to_root_clears_stack) {
    Navigator nav{named_route("home")};
    nav.push(named_route("detail"));
    nav.push(named_route("settings"));
    nav.push(named_route("extra"));

    nav.pop_to_root();
    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"home"});
    AURORA_TEST_CHECK_FALSE(nav.can_pop());
}

AURORA_TEST_CASE(navigator_route_changed_callback_counts) {
    int changed = 0;
    Navigator nav{named_route("home")};  // 构造期回调未设置，不计数。
    nav.set_on_route_changed([&changed]() { ++changed; });

    nav.push(named_route("detail"));          // 1
    nav.push_replacement(named_route("b"));   // 2
    AURORA_TEST_CHECK_TRUE(nav.pop());        // 3
    nav.push(named_route("c"));               // 4
    nav.pop_to_root();                        // 5
    AURORA_TEST_CHECK_FALSE(nav.pop());       // 根上拒绝，不通知
    AURORA_TEST_CHECK_EQ(changed, 5);
}

AURORA_TEST_CASE(navigator_max_depth_guard) {
    Navigator nav{named_route("home")};
    AURORA_TEST_CHECK_EQ(nav.max_depth(), AURORA_DEFAULT_MAX_NAV_DEPTH);

    nav.set_max_depth(2);
    AURORA_TEST_CHECK_EQ(nav.max_depth(), 2U);

    nav.push(named_route("a"));
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    nav.push(named_route("b"));  // 超限：经 Diagnostics 降级拒绝，栈不变。
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"a"});
}

AURORA_TEST_CASE(navigator_path_export_and_restore) {
    Navigator nav{named_route("home")};
    nav.push(named_route("detail"));
    nav.push(named_route("settings"));

    const std::vector<std::string> path = nav.path();
    AURORA_TEST_REQUIRE_EQ(path.size(), 3U);
    AURORA_TEST_CHECK_STREQ(path[0], "home");
    AURORA_TEST_CHECK_STREQ(path[1], "detail");
    AURORA_TEST_CHECK_STREQ(path[2], "settings");

    const std::function<Route(std::string)> build = [](std::string name) -> Route { return named_route(name); };
    nav.restore(std::vector<std::string>{"x", "y"}, build);
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"y"});

    const std::vector<std::string> restored = nav.path();
    AURORA_TEST_REQUIRE_EQ(restored.size(), 2U);
    AURORA_TEST_CHECK_STREQ(restored[0], "x");
    AURORA_TEST_CHECK_STREQ(restored[1], "y");
}

AURORA_TEST_CASE(navigator_restore_guards) {
    Navigator nav{named_route("home")};
    nav.push(named_route("detail"));
    const std::function<Route(std::string)> build = [](std::string name) -> Route { return named_route(name); };

    // 超过 max_depth 的 restore 整体拒绝，原栈保留。
    nav.set_max_depth(1);
    nav.restore(std::vector<std::string>{"x", "y"}, build);
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"detail"});

    // 空名称序列为无操作（不破坏当前栈）。
    nav.restore({}, build);
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"detail"});
}

AURORA_TEST_CASE(navigator_open_uri_split_and_registry) {
    const std::function<Route(const std::string &)> build = [](const std::string &name) -> Route {
        return named_route(name);
    };

    // 以 '/' 切分并丢弃空段。
    Navigator nav{named_route("home")};
    nav.open_uri("alpha//beta/", build);
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    const std::vector<std::string> path = nav.path();
    AURORA_TEST_REQUIRE_EQ(path.size(), 2U);
    AURORA_TEST_CHECK_STREQ(path[0], "alpha");
    AURORA_TEST_CHECK_STREQ(path[1], "beta");

    // 路由表缺失的名称段被跳过。
    RouteRegistry registry;
    registry["home"] = [](const std::string &name) -> Route { return named_route(name); };
    registry["detail"] = [](const std::string &name) -> Route { return named_route(name); };

    Navigator nav2{named_route("home")};
    nav2.open_uri("home/ghost/detail", registry);
    AURORA_TEST_CHECK_EQ(nav2.depth(), 2U);
    const std::vector<std::string> path2 = nav2.path();
    AURORA_TEST_REQUIRE_EQ(path2.size(), 2U);
    AURORA_TEST_CHECK_STREQ(path2[0], "home");
    AURORA_TEST_CHECK_STREQ(path2[1], "detail");
}

}  // namespace aurora::test_cases::utest_navigator
