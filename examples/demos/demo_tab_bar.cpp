// TabBar 控件 demo：三个选项卡切换内容，第三个可关闭。
#include "demo_common.h"

auto main() -> int {
    std::vector<au::Tab> tabs;
    tabs.push_back(au::Tab{ .label="首页", .content=au::Node{ au::Text{ "首页内容" } }, .closable=false });
    tabs.push_back(au::Tab{ .label="编辑", .content=au::Node{ au::Text{ "编辑内容" } }, .closable=false });
    tabs.push_back(au::Tab{ .label="预览", .content=au::Node{ au::Text{ "预览内容（可关闭）" } }, .closable=true });

    au::TabBar tab_bar{ std::move(tabs) };
    tab_bar.set_on_change([](int i) -> void { AURORA_LOG_INFO("demo", "切换到标签 ", i); });

    au::Node root = au::Column{
        GradientTitle{ "TabBar 控件" },
        gap(12),
        std::move(tab_bar),
    };
    return run_demo(Card{ std::move(root) }, "TabBar · Aurora Demo", 520.0f, 400.0f);
}
