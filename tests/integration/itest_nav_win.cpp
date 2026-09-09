/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h + include/aurora/navigation/{route,navigator,router}.h
/// 测试说明: 导航 + 窗口跨模块集成——HeadlessSurface 帧生命周期、create_window(HeadlessOptions)
/// 渲染根子树、run 有限帧循环、Route/Router/Navigator 栈语义与回调计数、
/// Navigator 切页后经 Window::present_root 逐页上屏

#include <memory>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/modifier/modifier.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/route.h"
#include "aurora/navigation/router.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_nav_win {

namespace {

/// 构造一个带纯色背景的页面（Text + background 修饰），作为路由根。
auto make_page(const std::string& label) -> Node {
    Text t{label};
    t.modifier.set(Modifier{}.background(Color::blue()));
    return Node{std::move(t)};
}

}  // namespace

// ---- Surface 帧生命周期（依赖 Headless 后端）----

#ifdef AURORA_BACKEND_HEADLESS

AURORA_TEST_CASE(headless_surface_frame_lifecycle) {
    auto surf = std::make_unique<HeadlessSurface>();
    const auto bf = surf->begin_frame(200, 150);
    AURORA_TEST_CHECK_TRUE(bf.ok());
    AURORA_TEST_CHECK_NOT_NULL(surf->painter().data());
    surf->painter().fill_rect(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 150.0F}}, Color::red());
    const auto pr = surf->present();
    AURORA_TEST_CHECK_TRUE(pr.ok());
    AURORA_TEST_CHECK_EQ(surf->frame_count(), 1);
}

AURORA_TEST_CASE(window_present_root_renders_subtree) {
    HeadlessOptions opts;
    opts.size = Size{.width = 240.0F, .height = 160.0F};
    opts.title = "test";
    auto res = create_window(opts);
    AURORA_TEST_REQUIRE_TRUE(res.ok());
    auto win = std::move(res.value());

    Node page = make_page("Hello");
    const auto r = win->present_root(page);
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 1);
    AURORA_TEST_CHECK_EQ(win->title(), std::string{"test"});
    win->set_title("renamed");
    AURORA_TEST_CHECK_EQ(win->title(), std::string{"renamed"});
}

AURORA_TEST_CASE(window_run_frame_loop_counts_max_frames) {
    HeadlessOptions opts;
    opts.size = Size{.width = 100.0F, .height = 100.0F};
    opts.title = "loop";
    auto res = create_window(opts);
    AURORA_TEST_REQUIRE_TRUE(res.ok());
    auto win = std::move(res.value());

    int frames = 0;
    Node page = make_page("x");
    win->run(
        [&]() -> void {
            ++frames;
            // 同页无状态变更：脏追踪会整帧跳过，强制重绘以模拟持续渲染的帧循环计数。
            win->force_full_redraw();
            (void)win->present_root(page);
        },
        4);
    AURORA_TEST_CHECK_EQ(frames, 4);
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 4);
}

#else

AURORA_TEST_CASE(headless_surface_frame_lifecycle) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：HeadlessSurface/create_window(HeadlessOptions) 未编译");
}

#endif

// ---- Route ----

AURORA_TEST_CASE(route_defaults_and_named_route) {
    const Route empty;
    AURORA_TEST_CHECK_TRUE(empty.empty());

    Route r{make_page("A"), "home"};
    AURORA_TEST_CHECK_FALSE(r.empty());
    AURORA_TEST_CHECK_EQ(r.name(), std::string{"home"});
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(r.root()));
    AURORA_TEST_CHECK_FALSE(r.transition().animated);  // 默认无转场
}

// ---- Router（路由表）----

AURORA_TEST_CASE(router_registry_build_and_missing) {
    Router router;
    router.register_route("home", []() -> Route { return Route{make_page("Home"), "home"}; });
    router.register_route("detail", []() -> Route { return Route{make_page("Detail"), "detail"}; });

    AURORA_TEST_CHECK_TRUE(router.has("home"));
    AURORA_TEST_CHECK_FALSE(router.has("missing"));

    const auto r = router.build("detail");
    AURORA_TEST_REQUIRE_TRUE(r.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(r.value().name(), std::string{"detail"});
    AURORA_TEST_CHECK_FALSE(router.build("missing").has_value());

    const Node root = router.build_root("home");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(root));
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(router.build_root("missing")));
}

// ---- Navigator（页面栈）----

AURORA_TEST_CASE(navigator_stack_ops_and_callback_counts) {
    Navigator nav{Route{make_page("Root"), "root"}};
    int changes = 0;
    nav.set_on_route_changed([&changes]() -> void { ++changes; });

    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_FALSE(nav.can_pop());

    nav.push(Route{make_page("Page2"), "p2"});
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_TRUE(nav.can_pop());
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"p2"});

    nav.push(Route{make_page("Page3"), "p3"});
    AURORA_TEST_CHECK_EQ(nav.depth(), 3U);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(nav.current_root()));

    AURORA_TEST_CHECK_TRUE(nav.pop());
    AURORA_TEST_CHECK_EQ(nav.depth(), 2U);
    AURORA_TEST_CHECK_EQ(nav.current().name(), std::string{"p2"});

    nav.pop_to_root();
    AURORA_TEST_CHECK_EQ(nav.depth(), 1U);
    AURORA_TEST_CHECK_FALSE(nav.pop());  // 仅剩根，拒绝
    // push(p2)、push(p3)、pop、popToRoot 共 4 次（构造期回调未设置，不计数）。
    AURORA_TEST_CHECK_EQ(changes, 4);
}

// ---- 集成：Navigator 切页 + Window 逐页上屏 ----

#ifdef AURORA_BACKEND_HEADLESS

AURORA_TEST_CASE(navigator_page_switch_presented_via_window) {
    Navigator nav{Route{make_page("Root"), "root"}};
    HeadlessOptions opts;
    opts.size = Size{.width = 200.0F, .height = 140.0F};
    opts.title = "nav";
    auto res = create_window(opts);
    AURORA_TEST_REQUIRE_TRUE(res.ok());
    auto win = std::move(res.value());

    Node cur = nav.current_root();
    auto r = win->present_root(cur);
    AURORA_TEST_CHECK_TRUE(r.ok());

    nav.push(Route{make_page("Second"), "second"});
    Node cur2 = nav.current_root();
    r = win->present_root(cur2);
    AURORA_TEST_CHECK_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(win->surface().frame_count(), 2);
}

#else

AURORA_TEST_CASE(navigator_page_switch_presented_via_window) {
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：HeadlessSurface/create_window(HeadlessOptions) 未编译");
}

#endif

}  // namespace aurora::test_cases::itest_nav_win
