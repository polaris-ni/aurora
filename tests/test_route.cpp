// test_route.cpp — navigation::Route 路由 1:1 测试：空路由 / 构造 / 拷贝 / 转场配置。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <cmath>
#include <string>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::Node;
using aurora::Route;
using aurora::RouteTransition;
using aurora::Spacer;
using aurora::TransitionKind;

static void test_route_empty() {
    const Route r;
    AURORA_TEST_CHECK_MSG(r.empty(), "Route: default ctor is empty");
    AURORA_TEST_CHECK_MSG(r.name().empty(), "Route: default name empty");
}

static void test_route_construct() {
    Route r{Node{Spacer{}}, "home"};
    AURORA_TEST_CHECK_MSG(!r.empty(), "Route: with root not empty");
    AURORA_TEST_CHECK_MSG(r.name() == "home", "Route: name preserved");
    AURORA_TEST_CHECK_MSG(static_cast<bool>(r.root()), "Route: root() is valid node");

    // 默认转场：不带动画、Fade、0.3s。
    const RouteTransition &t = r.transition();
    AURORA_TEST_CHECK_MSG(!t.animated, "Route: default transition not animated");
    AURORA_TEST_CHECK_MSG(t.kind == TransitionKind::Fade, "Route: default transition Fade");
    AURORA_TEST_CHECK_MSG(near_d(t.duration_seconds, 0.3), "Route: default transition 0.3s");
}

static void test_route_copy_and_transition() {
    RouteTransition tr;
    tr.animated = true;
    tr.kind = TransitionKind::Slide;
    tr.duration_seconds = 0.5;
    const Route r{Node{Spacer{}}, "page", tr};

    AURORA_TEST_CHECK_MSG(r.transition().animated, "Route: custom transition animated");
    AURORA_TEST_CHECK_MSG(r.transition().kind == TransitionKind::Slide, "Route: custom transition Slide");
    AURORA_TEST_CHECK_MSG(near_d(r.transition().duration_seconds, 0.5), "Route: custom transition 0.5s");

    // 拷贝共享同一棵树根。
    const Route &copy = r;
    AURORA_TEST_CHECK_MSG(copy.name() == "page", "Route: copy preserves name");
    AURORA_TEST_CHECK_MSG(!copy.empty(), "Route: copy not empty");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_route ===\n");
    test_route_empty();
    test_route_construct();
    test_route_copy_and_transition();
}
