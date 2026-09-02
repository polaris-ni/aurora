// MenuBar 控件 demo：File/Edit/Help 三个顶级菜单，含分隔符与禁用项。
#include "demo_common.h"

auto main() -> int {
    au::Menu file;
    file.title = "File";
    file.items.emplace_back("New", []() -> void { AURORA_LOG_INFO("demo", "New"); });
    file.items.emplace_back("Open...", []() -> void { AURORA_LOG_INFO("demo", "Open"); });
    file.items.push_back(au::MenuItem::separator_item());
    au::MenuItem locked{ "Locked Action" };
    locked.enabled = false;
    file.items.push_back(locked);

    au::Menu edit;
    edit.title = "Edit";
    au::MenuItem copy{ "Copy", []() -> void { AURORA_LOG_INFO("demo", "Copy"); } };
    copy.shortcut_text = "Ctrl+C";
    edit.items.push_back(copy);

    au::Menu help;
    help.title = "Help";
    help.items.emplace_back("About Aurora", []() -> void { AURORA_LOG_INFO("demo", "About"); });

    std::vector<au::Menu> menus;
    menus.push_back(std::move(file));
    menus.push_back(std::move(edit));
    menus.push_back(std::move(help));

    au::Node root = au::Column{
        au::MenuBar{ std::move(menus) },
        gap(12),
         GradientTitle{ "MenuBar" },
        au::Text{ "Click top menu to expand dropdown" },
    };
    return run_demo(std::move(root), "MenuBar · Aurora Demo", 520.0f, 360.0f);
}
