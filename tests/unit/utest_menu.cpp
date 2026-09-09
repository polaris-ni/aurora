/// 测试类型: unit
/// 目标单元: include/aurora/app/menu.h
/// 测试说明: 覆盖 MenuItem 声明式数据模型——默认字段值、带动作构造与回调触发、
/// 分隔符工厂、子菜单判定与条目树构建/递归遍历、checkable/enabled/shortcut_text 等状态字段、
/// 值语义复制保留回调

#include <functional>
#include <string>
#include <vector>

#include "aurora/app/menu.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_menu {

AURORA_TEST_CASE(default_item_fields) {
    const MenuItem item;

    AURORA_TEST_CHECK_TRUE(item.label.empty());
    AURORA_TEST_CHECK_FALSE(item.on_click != nullptr);
    AURORA_TEST_CHECK_TRUE(item.children.empty());
    AURORA_TEST_CHECK_FALSE(item.separator);
    AURORA_TEST_CHECK_FALSE(item.checkable);
    AURORA_TEST_CHECK_FALSE(item.checked);
    AURORA_TEST_CHECK_TRUE(item.enabled);
    AURORA_TEST_CHECK_TRUE(item.shortcut_text.empty());
    AURORA_TEST_CHECK_TRUE(item.icon.empty());
    AURORA_TEST_CHECK_FALSE(item.is_submenu());
}

AURORA_TEST_CASE(labeled_item_invokes_action) {
    int fired = 0;
    MenuItem item("Open", [&fired]() -> void { ++fired; });

    AURORA_TEST_CHECK_EQ(item.label, std::string{"Open"});
    AURORA_TEST_CHECK_TRUE(item.enabled);
    item.on_click();
    AURORA_TEST_CHECK_EQ(fired, 1);

    // 不带动作构造：回调为空。
    MenuItem plain("About");
    AURORA_TEST_CHECK_EQ(plain.label, std::string{"About"});
    AURORA_TEST_CHECK_FALSE(plain.on_click != nullptr);
}

AURORA_TEST_CASE(separator_item_factory) {
    const MenuItem sep = MenuItem::separator_item();

    AURORA_TEST_CHECK_TRUE(sep.separator);
    AURORA_TEST_CHECK_TRUE(sep.label.empty());
    AURORA_TEST_CHECK_FALSE(sep.is_submenu());
    AURORA_TEST_CHECK_FALSE(sep.on_click != nullptr);
}

AURORA_TEST_CASE(submenu_detection_and_child_order) {
    MenuItem file("File");
    file.children.emplace_back("New");
    file.children.emplace_back("Open");

    // 有 children 即判为子菜单；条目按插入顺序保存。
    AURORA_TEST_CHECK_TRUE(file.is_submenu());
    AURORA_TEST_CHECK_EQ(file.children.size(), 2U);
    AURORA_TEST_CHECK_EQ(file.children[0].label, std::string{"New"});
    AURORA_TEST_CHECK_EQ(file.children[1].label, std::string{"Open"});

    // 子项自身不是子菜单。
    AURORA_TEST_CHECK_FALSE(file.children[0].is_submenu());

    // 无 children 的普通项不是子菜单。
    MenuItem plain("About");
    AURORA_TEST_CHECK_FALSE(plain.is_submenu());
}

AURORA_TEST_CASE(nested_menu_tree_traversal) {
    // 声明式构建两级菜单树：File{New, sep, Exit}、Edit{Undo, sep, Redo}、Help{About}。
    MenuItem file("File");
    file.children.emplace_back("New");
    file.children.push_back(MenuItem::separator_item());
    file.children.emplace_back("Exit");

    MenuItem edit("Edit");
    edit.children.emplace_back("Undo");
    edit.children.push_back(MenuItem::separator_item());
    edit.children.emplace_back("Redo");

    MenuItem help("Help");
    help.children.emplace_back("About");

    // 三级子菜单：Window{File 副本} 验证任意深度嵌套。
    MenuItem window("Window");
    window.children.push_back(file);

    // 递归统计：叶子数 / 分隔符数。
    const std::function<int(const MenuItem&)> count_all = [&](const MenuItem& item) -> int {
        int n = 1;
        for (const auto& c : item.children) {
            n += count_all(c);
        }
        return n;
    };
    const std::function<int(const MenuItem&)> count_separators = [&](const MenuItem& item) -> int {
        int n = item.separator ? 1 : 0;
        for (const auto& c : item.children) {
            n += count_separators(c);
        }
        return n;
    };

    AURORA_TEST_CHECK_EQ(count_all(file), 4);  // File + New + sep + Exit
    AURORA_TEST_CHECK_EQ(count_all(edit), 4);  // Edit + Undo + sep + Redo
    AURORA_TEST_CHECK_EQ(count_all(window), 5);  // Window + File 子树
    AURORA_TEST_CHECK_EQ(count_separators(window), 1);
    AURORA_TEST_CHECK_EQ(count_all(help), 2);  // Help + About
}

AURORA_TEST_CASE(checkable_state_fields_roundtrip) {
    MenuItem toggle("Show Grid");
    AURORA_TEST_CHECK_FALSE(toggle.checkable);
    AURORA_TEST_CHECK_FALSE(toggle.checked);

    // 数据模型字段可直接赋值（渲染层消费勾选语义）。
    toggle.checkable = true;
    toggle.checked = true;
    toggle.shortcut_text = "Ctrl+G";
    toggle.icon = "grid";

    AURORA_TEST_CHECK_TRUE(toggle.checkable);
    AURORA_TEST_CHECK_TRUE(toggle.checked);
    AURORA_TEST_CHECK_EQ(toggle.shortcut_text, std::string{"Ctrl+G"});
    AURORA_TEST_CHECK_EQ(toggle.icon, std::string{"grid"});
}

AURORA_TEST_CASE(disabled_item_is_explicit_state) {
    int fired = 0;
    MenuItem item("Delete", [&fired]() -> void { ++fired; });
    item.enabled = false;
    item.shortcut_text = "Del";

    // enabled=false 为显式数据标记（灰显语义由渲染层实现）；
    // 数据模型本身不改写回调，仍可被宿主按 enabled 门控调用。
    AURORA_TEST_CHECK_FALSE(item.enabled);
    AURORA_TEST_CHECK_EQ(item.shortcut_text, std::string{"Del"});

    // 宿主侧门控语义：enabled=false 时不调用回调。
    if (item.enabled && item.on_click) {
        item.on_click();
    }
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(children_copy_preserves_callbacks) {
    // 值语义：把带回调的条目复制进另一菜单的 children，回调随副本生效。
    int fired = 0;
    MenuItem item("Copy", [&fired]() -> void { ++fired; });

    MenuItem menu("Edit");
    menu.children.push_back(item);

    item.label = "Changed";
    AURORA_TEST_CHECK_EQ(menu.children[0].label, std::string{"Copy"});  // 深拷贝互不影响
    AURORA_TEST_CHECK_TRUE(menu.children[0].on_click != nullptr);
    menu.children[0].on_click();
    AURORA_TEST_CHECK_EQ(fired, 1);
}

}  // namespace aurora::test_cases::utest_menu
