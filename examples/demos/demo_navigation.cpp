// 导航 demo：Route / Navigator / Router。
#include "demo_common.h"

auto main() -> int {
    au::Route home{ au::Column{ GradientTitle{ "Home" }, gap(8), au::Text{ au::LocalizedString{ "这是首页" } } },
                    "home" };
    au::Route about{ au::Column{ GradientTitle{ "About" }, gap(8), au::Text{ au::LocalizedString{ "这是关于页" } } },
                     "about" };

    au::Navigator nav{ home };
    nav.push(about);

    au::Router router;
    router.register_route(
        "home", []() -> au::Route { return au::Route{ au::Text{ au::LocalizedString{ "home route" } }, "home" }; });

    au::Node root = au::Column{
        GradientTitle{ "导航 / Navigation" },
        gap(12),
        au::Text{ au::LocalizedString{ "Navigator depth = " + std::to_string(nav.depth()) } },
        au::Text{ au::LocalizedString{ "Router has 'home' = " + std::string(router.has("home") ? "true" : "false") } },
        au::Text{ au::LocalizedString{ "current route = " + nav.current().name() } },
        au::Text{ au::LocalizedString{ "（可用 Navigator 在路由间跳转）" } },
    };
    return run_demo(Card{ std::move(root) }, "Navigation · Aurora Demo", 520.0f, 440.0f);
}
