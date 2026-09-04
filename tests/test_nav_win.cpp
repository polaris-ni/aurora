// Aurora window + navigation 模块单测：Surface / Window / Route / Router / Navigator
#include <memory>

#include "aurora/core/color.h"
#include "aurora/modifier/modifier.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/route.h"
#include "aurora/navigation/router.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "test_harness.h"

using aurora::Color;
using aurora::HeadlessOptions;
using aurora::HeadlessSurface;
using aurora::Modifier;
using aurora::Navigator;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Route;
using aurora::Router;
using aurora::RouteTransition;
using aurora::Size;
using aurora::Text;
using aurora::TextProps;

static auto make_page(const std::string &label) -> Node {
    Text t{TextProps{.content = label}};
    t.modifier.set(Modifier{}.background(Color::blue()));
    return Node{std::move(t)};
}

AURORA_TEST() {
    // ---- Surface ----
    {
        auto surf = std::make_unique<HeadlessSurface>("build/win_surface.png");
        auto b = surf->begin_frame(200, 150);
        AURORA_TEST_CHECK(static_cast<bool>(b));
        AURORA_TEST_CHECK(surf->painter().data() != nullptr);
        surf->painter().fill_rect(
            Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 150.0F}}, Color::red());
        auto p = surf->present();
        AURORA_TEST_CHECK(static_cast<bool>(p));
        AURORA_TEST_CHECK(surf->frame_count() == 1);
    }

    // ---- Window: presentRoot 渲染子树 ----
    {
        HeadlessOptions opts;
        opts.size = Size{.width = 240.0F, .height = 160.0F};
        opts.title = "test";
        opts.png_path = "build/win_root.png";
        auto res = create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto win = std::move(res.value());
        Node page = make_page("Hello");
        auto r = win->present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(win->surface().frame_count() == 1);
        AURORA_TEST_CHECK(win->title() == "test");
        win->set_title("renamed");
        AURORA_TEST_CHECK(win->title() == "renamed");
    }

    // ---- Window: run 帧循环 maxFrames ----
    {
        int frames = 0;
        HeadlessOptions opts;
        opts.size = Size{.width = 100.0F, .height = 100.0F};
        opts.title = "loop";
        opts.png_path = "build/win_loop.png";
        auto res = create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto win = std::move(res.value());
        Node page = make_page("x");
        win->run(
            [&]() -> void {
                ++frames;
                win->force_full_redraw();  // 同页无状态变更：模拟持续渲染的帧循环，强制重绘以计数
                (void)win->present_root(page);
            },
            4);
        AURORA_TEST_CHECK(frames == 4);
        AURORA_TEST_CHECK(win->surface().frame_count() == 4);
    }

    // ---- Route ----
    {
        Route empty;
        AURORA_TEST_CHECK(empty.empty());
        Route r{make_page("A"), "home"};
        AURORA_TEST_CHECK(!r.empty());
        AURORA_TEST_CHECK(r.name() == "home");
        AURORA_TEST_CHECK(static_cast<bool>(r.root()));
        const RouteTransition &tr = r.transition();
        AURORA_TEST_CHECK(!tr.animated);  // 默认无转场
    }

    // ---- Router ----
    {
        Router router;
        router.register_route("home", []() -> Route { return Route{make_page("Home"), "home"}; });
        router.register_route("detail", []() -> Route { return Route{make_page("Detail"), "detail"}; });
        AURORA_TEST_CHECK(router.has("home"));
        AURORA_TEST_CHECK(!router.has("missing"));
        auto r = router.build("detail");
        AURORA_TEST_REQUIRE(r.has_value());
        AURORA_TEST_CHECK(r->name() == "detail");
        auto none = router.build("missing");
        AURORA_TEST_CHECK(!none.has_value());
        Node root = router.build_root("home");
        AURORA_TEST_CHECK(static_cast<bool>(root));
        Node none_root = router.build_root("missing");
        AURORA_TEST_CHECK(!static_cast<bool>(none_root));
    }

    // ---- Navigator ----
    {
        Navigator nav{Route{make_page("Root"), "root"}};
        int changes = 0;
        nav.set_on_route_changed([&]() -> void { ++changes; });
        AURORA_TEST_CHECK(nav.depth() == 1);
        AURORA_TEST_CHECK(!nav.can_pop());
        nav.push(Route{make_page("Page2"), "p2"});
        AURORA_TEST_CHECK(nav.depth() == 2);
        AURORA_TEST_CHECK(nav.can_pop());
        AURORA_TEST_CHECK(nav.current().name() == "p2");
        nav.push(Route{make_page("Page3"), "p3"});
        AURORA_TEST_CHECK(nav.depth() == 3);
        AURORA_TEST_CHECK(static_cast<bool>(nav.current_root()));
        bool popped = nav.pop();
        AURORA_TEST_CHECK(popped);
        AURORA_TEST_CHECK(nav.depth() == 2);
        AURORA_TEST_CHECK(nav.current().name() == "p2");
        nav.pop_to_root();
        AURORA_TEST_CHECK(nav.depth() == 1);
        bool refused = nav.pop();  // 仅剩根，拒绝
        AURORA_TEST_CHECK(!refused);
        AURORA_TEST_CHECK(changes ==
                          4);  // push(p2),push(p3),pop,popToRoot（构造期 notify 发生在 setOnRouteChanged 之前）
    }

    // ---- Integration: Navigator + Window 切换并渲染 ----
    {
        Navigator nav{Route{make_page("Root"), "root"}};
        HeadlessOptions opts;
        opts.size = Size{.width = 200.0F, .height = 140.0F};
        opts.title = "nav";
        opts.png_path = "build/win_nav.png";
        auto res = create_window(opts);
        AURORA_TEST_CHECK(static_cast<bool>(res));
        auto win = std::move(res.value());
        Node cur = nav.current_root();
        auto r = win->present_root(cur);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        nav.push(Route{make_page("Second"), "second"});
        Node cur2 = nav.current_root();
        r = win->present_root(cur2);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK(win->surface().frame_count() == 2);
    }

    AURORA_LOG_INFO("test", "nav_win_test: ALL PASS");
}
