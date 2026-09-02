// SystemTray 验证：构造/析构/气泡/提示/状态查询（headless 记录路径）+ 右键上下文菜单。
#include <iostream>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::MenuItem;
using aurora::SystemTray;

AURORA_TEST() {
    {
        SystemTray tray("Aurora Test Tray");
        tray.show_balloon("Hello", "Tray balloon works");
        AURORA_TEST_CHECK_MSG(tray.last_balloon_message() == "Tray balloon works", "balloon recorded");

        tray.set_title("Updated Tray Title");
        tray.show_balloon("Info", "second balloon");
        AURORA_TEST_CHECK_MSG(tray.last_balloon_message() == "second balloon", "second balloon recorded");
        // tray 出作用域自动析构，移除图标（headless 无操作）
    }
    // 析构后应仍可访问 last_balloon_message（拷贝记忆）
    // Already out of scope — local destroyed. OK.

    // ---- 右键上下文菜单测试 ----
    {
        SystemTray tray("Menu Tray");

        // 默认无菜单项
        AURORA_TEST_CHECK_MSG(tray.context_menu_items().empty(), "default context menu empty");

        // 设置菜单项
        int clicked = 0;
        std::vector<MenuItem> items;
        items.emplace_back("Open", [&] -> void { ++clicked; });
        items.emplace_back("Settings", [&] -> void { clicked += 10; });
        items.push_back(MenuItem::separator_item());
        items.emplace_back("Quit", [&] -> void { clicked += 100; });
        tray.set_context_menu(std::move(items));

        AURORA_TEST_CHECK_MSG(tray.context_menu_items().size() == 4, "menu has 4 items (incl. separator)");
        AURORA_TEST_CHECK_MSG(!tray.context_menu_items()[0].separator, "first item not separator");
        AURORA_TEST_CHECK_MSG(tray.context_menu_items()[2].separator, "third item is separator");
        AURORA_TEST_CHECK_MSG(tray.context_menu_items()[0].label == "Open", "first item label");
        AURORA_TEST_CHECK_MSG(tray.context_menu_items()[3].label == "Quit", "last item label");

        // 验证回调可触发（headless 下菜单不弹出，但回调模型仍可验证）
        tray.context_menu_items()[0].on_click();
        AURORA_TEST_CHECK_MSG(clicked == 1, "Open callback fired");
        tray.context_menu_items()[3].on_click();
        AURORA_TEST_CHECK_MSG(clicked == 101, "Quit callback fired");

        // 子菜单测试
        SystemTray tray2("Submenu Tray");
        std::vector<MenuItem> sub_items;
        sub_items.emplace_back("Sub Item 1");
        sub_items.emplace_back("Sub Item 2");
        MenuItem parent("Parent");
        parent.children = std::move(sub_items);
        AURORA_TEST_CHECK_MSG(parent.is_submenu(), "parent is submenu");
        AURORA_TEST_CHECK_MSG(parent.children.size() == 2, "submenu has 2 items");

        std::vector<MenuItem> root_items;
        root_items.push_back(std::move(parent));
        tray2.set_context_menu(std::move(root_items));
        AURORA_TEST_CHECK_MSG(tray2.context_menu_items().size() == 1, "root has 1 item");
        AURORA_TEST_CHECK_MSG(tray2.context_menu_items()[0].is_submenu(), "root item is submenu");
    }
}
