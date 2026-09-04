// TabBar 控件 demo：三个选项卡切换内容，第三个可关闭。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    std::vector<au::Tab> tabs;
    tabs.push_back(au::Tab{.label = "Home", .content = au::Node{au::Text{"Home content"}}, .closable = false});
    tabs.push_back(au::Tab{.label = "Edit", .content = au::Node{au::Text{"Edit content"}}, .closable = false});
    tabs.push_back(
        au::Tab{.label = "Preview", .content = au::Node{au::Text{"Preview content (closable)"}}, .closable = true});

    au::TabBar tab_bar{std::move(tabs)};
    tab_bar.set_on_change([](int i) -> void { AURORA_LOG_INFO("demo", "Switch to tab ", i); });

    au::Node root = au::Column{
        GradientTitle{"TabBar widget"},
        gap(12),
        std::move(tab_bar),
    };
    return run_demo(Card{std::move(root)}, "TabBar · Aurora Demo", 520.0F, 400.0F);
}