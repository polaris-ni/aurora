// 深层链接（URI 字符串解析）演示。
//
// 维护一个 RouteRegistry（名称 → 路由构造器），通过 au::NavigatorHost::open_uri("home/detail/...")
// 按 '/' 切分名称序列并直接重建整栈，展示「一处 URI 跳多级」的深层链接能力。
// 用 au::Application 驱动 au::Animator，使转场（如 open_uri 后的页面切换）正常演进。
#include <memory>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Animator anim;
    auto host = std::make_shared<au::NavigatorHost>(anim);

    au::RouteRegistry registry;
    registry["home"] = [](const std::string &) -> au::Route {
        au::Text t{"Home"};
        return au::Route{au::Column{au::ColumnProps{.children = {std::move(t)}}}, "home"};
    };
    registry["detail"] = [](const std::string &) -> au::Route {
        au::Text t{"Detail"};
        return au::Route{au::Column{au::ColumnProps{.children = {std::move(t)}}}, "detail"};
    };
    registry["settings"] = [](const std::string &) -> au::Route {
        au::Text t{"Settings"};
        return au::Route{au::Column{au::ColumnProps{.children = {std::move(t)}}}, "settings"};
    };

    // 初始栈：home
    host->push(registry.at("home")("home"));

    // 控制页：按钮通过 open_uri 深层跳转（重建整栈）。
    auto make_controls = [host, registry]() -> au::Node {
        auto jump = au::Button{"Open home/detail/settings (deep link)"};
        // NOLINTNEXTLINE(bugprone-exception-escape) 误报：转入 std::function 的 lambda
        // 被本检查一律判为「不应抛出」（operator() 非 noexcept，static_assert 已证）
        jump.set_on_click([host, registry]() -> void { host->open_uri("home/detail/settings", registry); });
        auto reset = au::Button{"Open home (deep link)"};
        // NOLINTNEXTLINE(bugprone-exception-escape) 误报：转入 std::function 的 lambda
        // 被本检查一律判为「不应抛出」（operator() 非 noexcept，static_assert 已证）
        reset.set_on_click([host, registry]() -> void { host->open_uri("home", registry); });
        return au::Column{au::ColumnProps{.children = {std::move(jump), gap(12.0F), std::move(reset)}}};
    };
    host->push(au::Route{make_controls(), "controls"});

    au::Scene scene{au::Node{host}};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 420.0F, .height = 320.0F};
    opts.title = "Navigator deep link";
    auto win_res = create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    app.set_on_frame([&anim]() -> void { anim.tick(1.0 / 60.0); });
    app.run();
    return 0;
}