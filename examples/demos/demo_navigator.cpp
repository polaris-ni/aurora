// 深层链接（URI 字符串解析）演示。
//
// 维护一个 RouteRegistry（名称 → 路由构造器），通过 NavigatorHost::open_uri("home/detail/...")
// 按 '/' 切分名称序列并直接重建整栈，展示「一处 URI 跳多级」的深层链接能力。
// 用 Application 驱动 Animator，使转场（如 open_uri 后的页面切换）正常演进。
#include <memory>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"

#include "demo_common.h"

using namespace au;

auto main() -> int {
    Animator anim;
    auto host = std::make_shared<NavigatorHost>(anim);

    RouteRegistry registry;
    registry["home"] = [](const std::string &) {
        Text t{ "Home" };
        return Route{ Column{ ColumnProps{ .children = { std::move(t) } } }, "home" };
    };
    registry["detail"] = [](const std::string &) {
        Text t{ "Detail" };
        return Route{ Column{ ColumnProps{ .children = { std::move(t) } } }, "detail" };
    };
    registry["settings"] = [](const std::string &) {
        Text t{ "Settings" };
        return Route{ Column{ ColumnProps{ .children = { std::move(t) } } }, "settings" };
    };

    // 初始栈：home
    host->push(registry.at("home")("home"));

    // 控制页：按钮通过 open_uri 深层跳转（重建整栈）。
    auto make_controls = [host, registry]() -> Node {
        auto jump = Button{ "Open home/detail/settings (deep link)" };
        jump.set_on_click([host, registry]() { (void)host->open_uri("home/detail/settings", registry); });
        auto reset = Button{ "Open home (deep link)" };
        reset.set_on_click([host, registry]() { (void)host->open_uri("home", registry); });
        return Column{ ColumnProps{ .children = { std::move(jump), gap(12.0f), std::move(reset) } } };
    };
    host->push(Route{ make_controls(), "controls" });

    Scene scene{ Node{ host } };
    WindowOptions wopts;
    wopts.size = Size{ 420.0f, 320.0f };
    wopts.title = "Navigator 深层链接";
    auto win_res = create_native_window(wopts);
    Application app{ std::move(scene), win_res ? std::move(win_res.value()) : nullptr, wopts };
    app.set_on_frame([&anim]() { anim.tick(1.0 / 60.0); });
    app.run();
    return 0;
}
