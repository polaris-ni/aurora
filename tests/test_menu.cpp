// MERGED TEST 目标源单元：app/menu.h（MenuItem 声明式菜单数据模型）。
//
// API 覆盖映射：
//   MenuItem()                        -> test_default_fields
//   MenuItem(text, action) / label    -> test_explicit_ctor
//   MenuItem::on_click 调用           -> test_on_click_invoked
//   MenuItem::separator_item()        -> test_separator_item
//   MenuItem::is_submenu()/children   -> test_submenu
//   checkable/checked/enabled 字段    -> test_state_flags
//   shortcut_text/icon                -> test_shortcut_and_icon

#include <string>

#include "aurora/app/menu.h"

#include "test_harness.h"

using aurora::MenuItem;

namespace {

void test_default_fields() {
    const MenuItem item;
    AURORA_TEST_CHECK(item.label.empty());
    AURORA_TEST_CHECK(!item.on_click);
    AURORA_TEST_CHECK(item.children.empty());
    AURORA_TEST_CHECK_FALSE(item.separator);
    AURORA_TEST_CHECK_FALSE(item.checkable);
    AURORA_TEST_CHECK_FALSE(item.checked);
    AURORA_TEST_CHECK(item.enabled);
    AURORA_TEST_CHECK(item.shortcut_text.empty());
    AURORA_TEST_CHECK(item.icon.empty());
    AURORA_TEST_CHECK_FALSE(item.is_submenu());
}

void test_explicit_ctor() {
    bool clicked = false;
    const MenuItem item{ "打开", [&clicked] -> void { clicked = true; } };
    AURORA_TEST_CHECK_MSG(item.label == "打开", "label forwarded");
    AURORA_TEST_CHECK(item.enabled);
    item.on_click();
    AURORA_TEST_CHECK(clicked);
}

void test_on_click_invoked() {
    int count = 0;
    const MenuItem item{ "计数", [&count] -> void { ++count; } };
    item.on_click();
    item.on_click();
    AURORA_TEST_CHECK_EQ(count, 2);
}

void test_separator_item() {
    const auto sep = MenuItem::separator_item();
    AURORA_TEST_CHECK(sep.separator);
    AURORA_TEST_CHECK(sep.label.empty());
    AURORA_TEST_CHECK(!sep.on_click);
    AURORA_TEST_CHECK_FALSE(sep.is_submenu());
}

void test_submenu() {
    MenuItem sub{ "子项" };
    MenuItem parent{ "父项" };
    parent.children.push_back(sub);
    parent.children.push_back(MenuItem::separator_item());
    AURORA_TEST_CHECK(parent.is_submenu());
    AURORA_TEST_CHECK_EQ(parent.children.size(), static_cast<size_t>(2));
    AURORA_TEST_CHECK_FALSE(parent.children[0].is_submenu());

    MenuItem nested{ "嵌套" };
    nested.children.emplace_back("更深");
    parent.children.push_back(nested);
    AURORA_TEST_CHECK(parent.children[2].is_submenu());
}

void test_state_flags() {
    MenuItem item{ "勾选项" };
    item.checkable = true;
    item.checked = true;
    item.enabled = false;
    item.shortcut_text = "Ctrl+O";
    item.icon = "open";
    AURORA_TEST_CHECK(item.checkable && item.checked && !item.enabled);
    AURORA_TEST_CHECK(item.shortcut_text == "Ctrl+O");
    AURORA_TEST_CHECK(item.icon == "open");
    // disabled 不影响数据模型语义（灰显是渲染层关注点）
    AURORA_TEST_CHECK(!item.on_click);
}

void test_shortcut_and_icon() {
    MenuItem item{ "另存为", {} };
    item.shortcut_text = "Shift+Ctrl+S";
    AURORA_TEST_CHECK_MSG(item.shortcut_text == "Shift+Ctrl+S", "shortcut text kept");
    item.icon.clear();
    AURORA_TEST_CHECK(item.icon.empty());
}

} // namespace

AURORA_TEST() {
    test_default_fields();
    test_explicit_ctor();
    test_on_click_invoked();
    test_separator_item();
    test_submenu();
    test_state_flags();
    test_shortcut_and_icon();
}
