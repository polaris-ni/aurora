// TabBar 控件 demo：三个选项卡切换内容，第三个可关闭。
#include "demo_common.h"

auto main() -> int {
    std::vector<au::Tab> tabs;
    tabs.push_back(au::Tab{ .label = "Home", .content = au::Node{ au::Text{ "Home content" } }, .closable = false });
    tabs.push_back(au::Tab{ .label = "Edit", .content = au::Node{ au::Text{ "Edit content" } }, .closable = false });
    tabs.push_back(
        au::Tab{ .label = "Preview", .content = au::Node{ au::Text{ "Preview content (closable)" } }, .closable = true });

    au::TabBar tab_bar{ std::move(tabs) };
    tab_bar.set_on_change([](int i) -> void { AURORA_LOG_INFO("demo", "Switch to tab ", i); });

    au::Node root = au::Column{
        GradientTitle{ "TabBar widget" },
        gap(12),
        std::move(tab_bar),
    };
    return run_demo(Card{ std::move(root) }, "TabBar · Aurora Demo", 520.0f, 400.0f);
}
