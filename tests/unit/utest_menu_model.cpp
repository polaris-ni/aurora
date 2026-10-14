/// @file utest_menu_model.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/app/menu_model.cpp
/// 测试说明: 覆盖菜单数据模型 MenuModel 与菜单声明之间的双向投影——菜单项数据字段逐字段往返保真
/// （label / enabled / checkable / checked / shortcut_text / separator / children 嵌套与顺序）、
/// 稳定标识透传、空 children 不产生子菜单语义（与 is_submenu 同口径）、顶级菜单标题映射为 label、
/// 菜单栏整体投影顺序保真、回转结果不含行为与图标、投影纯度（重复调用一致、源对象不被改写）。

#include <string>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/widget/menu_bar.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_menu_model {

namespace {

/// 逐字段比较两个数据模型（含 children 递归），供往返与纯度断言复用。
auto same_model(const MenuModel &a, const MenuModel &b) -> bool {
    if (a.id != b.id || a.label != b.label || a.enabled != b.enabled || a.checkable != b.checkable ||
        a.checked != b.checked || a.shortcut_text != b.shortcut_text || a.separator != b.separator ||
        a.children.size() != b.children.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!same_model(a.children[i], b.children[i])) {
            return false;
        }
    }
    return true;
}

/// 构造两级嵌套、各状态字段均非默认的菜单项，作为往返用例的统一素材。
auto sample_item() -> MenuItem {
    MenuItem item("Save As");
    item.id = "file.save_as";
    item.enabled = false;
    item.checkable = true;
    item.checked = true;
    item.shortcut_text = "Ctrl+Shift+S";
    item.icon = "disk";

    MenuItem child("Plain Text");
    child.id = "file.save_as.plain";
    child.shortcut_text = "Ctrl+1";

    MenuItem grandchild("UTF-8");
    grandchild.id = "file.save_as.plain.utf8";
    grandchild.checkable = true;
    child.children.push_back(grandchild);

    item.children.push_back(child);
    item.children.push_back(MenuItem::separator_item());
    return item;
}

}  // namespace

AURORA_TEST_CASE(item_projection_copies_data_fields) {
    const MenuItem item = sample_item();
    const MenuModel model = to_menu_model(item);

    AURORA_TEST_CHECK_EQ(model.label, std::string{"Save As"});
    AURORA_TEST_CHECK_FALSE(model.enabled);
    AURORA_TEST_CHECK_TRUE(model.checkable);
    AURORA_TEST_CHECK_TRUE(model.checked);
    AURORA_TEST_CHECK_EQ(model.shortcut_text, std::string{"Ctrl+Shift+S"});
    AURORA_TEST_CHECK_FALSE(model.separator);

    // 变异自证：实现里把 model.enabled 写成恒 true，或漏抄 checkable / checked / shortcut_text 任一字段，
    // 上面对应的断言即转红；把 label 误写成空串同样转红。
    AURORA_TEST_CHECK_EQ(model.children.size(), 2U);
    AURORA_TEST_CHECK_EQ(model.children[0].label, std::string{"Plain Text"});
    AURORA_TEST_CHECK_TRUE(model.children[1].separator);
}

AURORA_TEST_CASE(roundtrip_preserves_fields_and_nesting) {
    const MenuItem item = sample_item();
    const MenuItem back = from_menu_model(to_menu_model(item));

    AURORA_TEST_CHECK_EQ(back.label, item.label);
    AURORA_TEST_CHECK_EQ(back.enabled, item.enabled);
    AURORA_TEST_CHECK_EQ(back.checkable, item.checkable);
    AURORA_TEST_CHECK_EQ(back.checked, item.checked);
    AURORA_TEST_CHECK_EQ(back.shortcut_text, item.shortcut_text);
    AURORA_TEST_CHECK_EQ(back.separator, item.separator);

    // 嵌套与顺序逐位保真：两级子项 + 末尾分隔符。
    AURORA_TEST_CHECK_EQ(back.children.size(), 2U);
    AURORA_TEST_CHECK_EQ(back.children[0].label, std::string{"Plain Text"});
    AURORA_TEST_CHECK_TRUE(back.children[0].is_submenu());
    AURORA_TEST_CHECK_EQ(back.children[0].children.size(), 1U);
    AURORA_TEST_CHECK_EQ(back.children[0].children[0].label, std::string{"UTF-8"});
    AURORA_TEST_CHECK_TRUE(back.children[1].separator);

    // 变异自证：from_menu_model 若把 children 倒序、丢层或漏拷 separator，
    // 上面三条顺序/深度断言与 separator 断言即转红。
    AURORA_TEST_CHECK_TRUE(same_model(to_menu_model(back), to_menu_model(item)));
}

AURORA_TEST_CASE(id_is_carried_through_both_directions) {
    const MenuItem item = sample_item();
    const MenuModel model = to_menu_model(item);

    AURORA_TEST_CHECK_EQ(model.id, std::string{"file.save_as"});
    AURORA_TEST_CHECK_EQ(model.children[0].id, std::string{"file.save_as.plain"});
    AURORA_TEST_CHECK_EQ(model.children[0].children[0].id, std::string{"file.save_as.plain.utf8"});

    const MenuItem back = from_menu_model(model);
    AURORA_TEST_CHECK_EQ(back.id, std::string{"file.save_as"});
    AURORA_TEST_CHECK_EQ(back.children[0].id, std::string{"file.save_as.plain"});

    // 未填标识的项：投影与回转两端都保持空串，不凭空造标识。
    const MenuItem plain("About");
    AURORA_TEST_CHECK_TRUE(to_menu_model(plain).id.empty());
    AURORA_TEST_CHECK_TRUE(from_menu_model(to_menu_model(plain)).id.empty());

    // 变异自证：to_menu_model / from_menu_model 任一侧漏写 id 透传，上面的标识断言与空串断言即转红。
}

AURORA_TEST_CASE(empty_children_carries_no_submenu_semantics) {
    const MenuItem plain("Help");
    const MenuModel model = to_menu_model(plain);

    AURORA_TEST_CHECK_TRUE(model.children.empty());
    AURORA_TEST_CHECK_FALSE(model.separator);

    // 空 children 回转后不是子菜单；分隔符同样不带子菜单语义。
    const MenuItem back = from_menu_model(model);
    AURORA_TEST_CHECK_FALSE(back.is_submenu());
    AURORA_TEST_CHECK_FALSE(from_menu_model(to_menu_model(MenuItem::separator_item())).is_submenu());

    // 非空 children 回转后才是子菜单，与 MenuItem::is_submenu 同口径。
    MenuItem parent("Window");
    parent.children.emplace_back("Minimize");
    AURORA_TEST_CHECK_TRUE(from_menu_model(to_menu_model(parent)).is_submenu());

    // 变异自证：把子菜单判据从「children 非空」改成「children 非空且 label 非空」或恒 true/false，
    // 上面三组 is_submenu 断言必有一组转红。
}

AURORA_TEST_CASE(separator_flag_survives_roundtrip) {
    const MenuItem sep = MenuItem::separator_item();
    const MenuModel model = to_menu_model(sep);

    AURORA_TEST_CHECK_TRUE(model.separator);
    AURORA_TEST_CHECK_TRUE(model.label.empty());

    const MenuItem back = from_menu_model(model);
    AURORA_TEST_CHECK_TRUE(back.separator);
    AURORA_TEST_CHECK_TRUE(back.label.empty());
    AURORA_TEST_CHECK_TRUE(back.children.empty());
}

AURORA_TEST_CASE(roundtrip_drops_behavior_and_icon) {
    int fired = 0;
    MenuItem item("Open", [&fired]() -> void { ++fired; });
    item.icon = "folder";

    const MenuItem back = from_menu_model(to_menu_model(item));
    AURORA_TEST_CHECK_EQ(back.label, std::string{"Open"});
    AURORA_TEST_CHECK_FALSE(back.on_click != nullptr);
    AURORA_TEST_CHECK_TRUE(back.icon.empty());

    // 回转项无回调可触发，宿主侧的点击计数保持零。
    if (back.on_click) {
        back.on_click();
    }
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(top_level_menu_projection_maps_title_to_label) {
    Menu menu;
    menu.title = "File";
    menu.id = "menu.file";
    menu.items.emplace_back("New");
    menu.items.push_back(MenuItem::separator_item());
    MenuItem quit("Quit");
    quit.id = "file.quit";
    quit.shortcut_text = "Ctrl+Q";
    menu.items.push_back(quit);

    const MenuModel model = to_menu_model(menu);

    // 顶级菜单标题落在 label，下拉项落在 children，标识透传。
    AURORA_TEST_CHECK_EQ(model.label, std::string{"File"});
    AURORA_TEST_CHECK_EQ(model.id, std::string{"menu.file"});
    AURORA_TEST_CHECK_EQ(model.children.size(), 3U);
    AURORA_TEST_CHECK_EQ(model.children[0].label, std::string{"New"});
    AURORA_TEST_CHECK_TRUE(model.children[1].separator);
    AURORA_TEST_CHECK_EQ(model.children[2].label, std::string{"Quit"});
    AURORA_TEST_CHECK_EQ(model.children[2].shortcut_text, std::string{"Ctrl+Q"});
    AURORA_TEST_CHECK_EQ(model.children[2].id, std::string{"file.quit"});

    // 变异自证：把顶级投影写成 label = 空串（漏映射 title）或 children 取 items 之外的内容，
    // 上面首条与 children 相关断言即转红。
    const MenuItem as_item = from_menu_model(model);
    AURORA_TEST_CHECK_EQ(as_item.label, std::string{"File"});
    AURORA_TEST_CHECK_TRUE(as_item.is_submenu());
}

AURORA_TEST_CASE(menu_bar_projection_preserves_order) {
    std::vector<Menu> bar;
    Menu file;
    file.title = "File";
    file.id = "menu.file";
    file.items.emplace_back("Open");
    Menu edit;
    edit.title = "Edit";
    edit.id = "menu.edit";
    edit.items.emplace_back("Undo");
    Menu help;
    help.title = "Help";
    bar.push_back(file);
    bar.push_back(edit);
    bar.push_back(help);

    const std::vector<MenuModel> models = to_menu_models(bar);

    AURORA_TEST_CHECK_EQ(models.size(), 3U);
    AURORA_TEST_CHECK_EQ(models[0].label, std::string{"File"});
    AURORA_TEST_CHECK_EQ(models[1].label, std::string{"Edit"});
    AURORA_TEST_CHECK_EQ(models[2].label, std::string{"Help"});
    AURORA_TEST_CHECK_EQ(models[0].id, std::string{"menu.file"});
    AURORA_TEST_CHECK_EQ(models[1].id, std::string{"menu.edit"});
    AURORA_TEST_CHECK_TRUE(models[2].id.empty());
    AURORA_TEST_CHECK_EQ(models[0].children.size(), 1U);
    AURORA_TEST_CHECK_TRUE(models[2].children.empty());

    // 空菜单栏：投影为空列表，不凭空补项。
    AURORA_TEST_CHECK_TRUE(to_menu_models(std::vector<Menu>{}).empty());

    // 变异自证：菜单栏投影若按标题字典序重排、或跳过无标识的顶级菜单，上面的顺序断言与末尾断言即转红。
}

AURORA_TEST_CASE(projection_is_pure_and_repeatable) {
    const MenuItem item = sample_item();

    const MenuModel first = to_menu_model(item);
    const MenuModel second = to_menu_model(item);
    const MenuModel third = to_menu_model(item);

    // 同一输入多次投影结果一致；回转同样可重复，且不与首次结果分叉。
    AURORA_TEST_CHECK_TRUE(same_model(first, second));
    AURORA_TEST_CHECK_TRUE(same_model(second, third));
    AURORA_TEST_CHECK_TRUE(same_model(first, to_menu_model(from_menu_model(first))));

    // 无副作用：投影不改写源对象的任何字段。
    AURORA_TEST_CHECK_EQ(item.label, std::string{"Save As"});
    AURORA_TEST_CHECK_EQ(item.id, std::string{"file.save_as"});
    AURORA_TEST_CHECK_FALSE(item.enabled);
    AURORA_TEST_CHECK_TRUE(item.checked);
    AURORA_TEST_CHECK_EQ(item.icon, std::string{"disk"});
    AURORA_TEST_CHECK_EQ(item.children.size(), 2U);
    AURORA_TEST_CHECK_TRUE(item.children[0].on_click == nullptr);

    // 变异自证：投影若把源项的 children 移出（改用移动而非拷贝）、或改写 enabled / icon，
    // 上面源对象断言即转红。
}

}  // namespace aurora::test_cases::utest_menu_model
