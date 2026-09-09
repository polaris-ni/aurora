/// 测试类型: unit
/// 目标单元: include/aurora/navigation/router.h
/// 测试说明: 覆盖 Router 命名路由的登记/查询、按名构建（每次调用工厂产出全新树）、
/// 未知路由的 nullopt/空 Node 降级与同名重登记覆盖

#include <memory>
#include <string>
#include <utility>

#include "aurora/navigation/route.h"
#include "aurora/navigation/router.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_router {

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

}  // namespace

AURORA_TEST_CASE(router_register_and_has) {
    Router router;
    AURORA_TEST_CHECK_FALSE(router.has("home"));

    router.register_route("home", []() -> Route { return Route{Node{SolidBox{}}, "home"}; });
    AURORA_TEST_CHECK_TRUE(router.has("home"));
    AURORA_TEST_CHECK_FALSE(router.has("detail"));
}

AURORA_TEST_CASE(router_build_unknown_returns_nullopt) {
    const Router router;
    const auto route = router.build("ghost");
    AURORA_TEST_CHECK_FALSE(route.has_value());
}

AURORA_TEST_CASE(router_build_invokes_factory) {
    Router router;
    int built = 0;
    router.register_route("home", [&built]() -> Route {
        ++built;
        return Route{Node{SolidBox{}}, "home"};
    });

    const auto first = router.build("home");
    AURORA_TEST_REQUIRE_TRUE(first.has_value());
    const auto second = router.build("home");
    AURORA_TEST_REQUIRE_TRUE(second.has_value());
    AURORA_TEST_CHECK_EQ(built, 2);
}

AURORA_TEST_CASE(router_build_returns_fresh_tree_each_time) {
    Router router;
    router.register_route("home", []() -> Route { return Route{Node{SolidBox{}}, "home"}; });

    const auto a = router.build("home");
    const auto b = router.build("home");
    AURORA_TEST_REQUIRE_TRUE(a.has_value());
    AURORA_TEST_REQUIRE_TRUE(b.has_value());
    // 工厂模式：每次导航获得全新 widget 树，不在栈间共享可变实例。
    AURORA_TEST_CHECK_NE(&a->root().widget(), &b->root().widget());
}

AURORA_TEST_CASE(router_build_root_registered_and_unknown) {
    Router router;
    router.register_route("home", []() -> Route { return Route{Node{SolidBox{}}, "home"}; });

    const Node root = router.build_root("home");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(root));
    AURORA_TEST_CHECK_STREQ(root.widget().type_name(), "SolidBox");

    const Node missing = router.build_root("ghost");
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(missing));
}

AURORA_TEST_CASE(router_reregister_overrides_builder) {
    Router router;
    int first_builder = 0;
    int second_builder = 0;
    router.register_route("home", [&first_builder]() -> Route {
        ++first_builder;
        return Route{Node{SolidBox{}}, "first"};
    });
    router.register_route("home", [&second_builder]() -> Route {
        ++second_builder;
        return Route{Node{SolidBox{}}, "second"};
    });

    const auto route = router.build("home");
    AURORA_TEST_REQUIRE_TRUE(route.has_value());
    AURORA_TEST_CHECK_EQ(route->name(), std::string{"second"});
    AURORA_TEST_CHECK_EQ(first_builder, 0);
    AURORA_TEST_CHECK_EQ(second_builder, 1);
}

AURORA_TEST_CASE(router_build_preserves_route_payload) {
    Router router;
    router.register_route("detail", []() -> Route {
        return Route{Node{SolidBox{}}, "detail",
                     RouteTransition{.animated = true, .kind = TransitionKind::Slide, .duration_seconds = 0.4}};
    });

    const auto route = router.build("detail");
    AURORA_TEST_REQUIRE_TRUE(route.has_value());
    AURORA_TEST_CHECK_EQ(route->name(), std::string{"detail"});
    AURORA_TEST_CHECK_TRUE(route->transition().animated);
    AURORA_TEST_CHECK_TRUE(route->transition().kind == TransitionKind::Slide);
    AURORA_TEST_CHECK_NEAR(route->transition().duration_seconds, 0.4, 1e-4F);
}

}  // namespace aurora::test_cases::utest_router
