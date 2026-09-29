/// 测试类型: unit
/// 目标单元: include/aurora/app/system_tray.h
/// 测试说明: 覆盖 SystemTray 共用的 MenuItem 声明式菜单数据模型（默认值、分隔符、
/// 子菜单谓词、点击回调计数）与托盘配置纯逻辑（右键菜单存取、气泡正文记录、移动语义），
/// 并锁定托盘回调 lParam 两种编码（版本 4 打包 / 旧版整值）的分类规则；
/// Windows 下 SystemTray 构造即注册真实托盘图标，实例级用例以 SKIP 桩跳过

#include <string>
#include <vector>

#include "aurora/app/detail/tray_events.h"
#include "aurora/app/system_tray.h"
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_system_tray {

AURORA_TEST_CASE(menu_item_defaults_and_fields) {
    const aurora::MenuItem def;
    AURORA_TEST_CHECK_TRUE(def.label.empty());
    AURORA_TEST_CHECK_FALSE(def.on_click != nullptr);
    AURORA_TEST_CHECK_TRUE(def.children.empty());
    AURORA_TEST_CHECK_FALSE(def.separator);
    AURORA_TEST_CHECK_FALSE(def.checkable);
    AURORA_TEST_CHECK_FALSE(def.checked);
    AURORA_TEST_CHECK_TRUE(def.enabled);
    AURORA_TEST_CHECK_TRUE(def.shortcut_text.empty());
    AURORA_TEST_CHECK_TRUE(def.icon.empty());

    // 普通项构造：label + 可选回调。
    const aurora::MenuItem item{"Quit"};
    AURORA_TEST_CHECK_STREQ(item.label, "Quit");
    AURORA_TEST_CHECK_FALSE(item.separator);
}

AURORA_TEST_CASE(menu_item_separator_and_submenu_predicates) {
    // 分隔符工厂：separator 置位、无文本。
    const auto sep = aurora::MenuItem::separator_item();
    AURORA_TEST_CHECK_TRUE(sep.separator);
    AURORA_TEST_CHECK_TRUE(sep.label.empty());
    AURORA_TEST_CHECK_FALSE(sep.is_submenu());

    // 子菜单谓词：children 非空即子菜单。
    aurora::MenuItem parent{"File"};
    const aurora::MenuItem open{"Open"};
    parent.children.push_back(open);
    AURORA_TEST_CHECK_TRUE(parent.is_submenu());
    AURORA_TEST_CHECK_STREQ(parent.children[0].label, "Open");

    const aurora::MenuItem leaf{"Quit"};
    AURORA_TEST_CHECK_FALSE(leaf.is_submenu());
}

AURORA_TEST_CASE(menu_item_click_callback_counted) {
    // 栈局部计数器：回调被手动触发时按次递增（不涉及真实托盘）。
    int clicks = 0;
    aurora::MenuItem item{"quit", [&clicks]() -> void { ++clicks; }};
    item.on_click();
    item.on_click();
    AURORA_TEST_CHECK_EQ(clicks, 2);

    // 子菜单项回调独立计数。
    int sub_clicks = 0;
    aurora::MenuItem parent{"File", [&sub_clicks]() -> void { ++sub_clicks; }};
    parent.on_click();
    AURORA_TEST_CHECK_EQ(sub_clicks, 1);
    AURORA_TEST_CHECK_EQ(clicks, 2);
}

AURORA_TEST_CASE(tray_context_menu_roundtrip) {
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_SKIP(
        "the Windows SystemTray registers a real tray icon (Shell_NotifyIcon) in its constructor; "
        "unit tests do not touch it");
#else
    SystemTray tray("utest-tray");
    AURORA_TEST_CHECK_TRUE(tray.context_menu_items().empty());

    std::vector<aurora::MenuItem> items;
    items.emplace_back("Open");
    items.push_back(aurora::MenuItem::separator_item());
    items.emplace_back("Quit");
    tray.set_context_menu(items);

    const auto &stored = tray.context_menu_items();
    AURORA_TEST_REQUIRE_EQ(stored.size(), 3U);
    AURORA_TEST_CHECK_STREQ(stored[0].label, "Open");
    AURORA_TEST_CHECK_FALSE(stored[0].separator);
    AURORA_TEST_CHECK_TRUE(stored[1].separator);
    AURORA_TEST_CHECK_STREQ(stored[2].label, "Quit");
    // 按值存储：源向量保持完整。
    AURORA_TEST_CHECK_EQ(items.size(), 3U);
#endif
}

AURORA_TEST_CASE(tray_balloon_message_recorded) {
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_SKIP(
        "the Windows SystemTray registers a real tray icon (Shell_NotifyIcon) in its constructor; "
        "unit tests do not touch it");
#else
    SystemTray tray("utest-tray");
    AURORA_TEST_CHECK_TRUE(tray.last_balloon_message().empty());

    // show_balloon 记录最近一次正文（headless 下即可查询，不弹真实气泡）。
    tray.show_balloon("Title", "hello");
    AURORA_TEST_CHECK_STREQ(tray.last_balloon_message(), "hello");

    // 空正文覆盖旧值。
    tray.show_balloon("Title2", "");
    AURORA_TEST_CHECK_TRUE(tray.last_balloon_message().empty());
#endif
}

AURORA_TEST_CASE(tray_move_preserves_state) {
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_SKIP(
        "the Windows SystemTray registers a real tray icon (Shell_NotifyIcon) in its constructor; "
        "unit tests do not touch it");
#else
    SystemTray tray("utest-tray");
    tray.set_title("renamed");
    tray.show_balloon("t", "msg");
    int activated = 0;
    tray.on_activate([&activated] { ++activated; });

    // 移动构造：状态字段随对象转移。
    SystemTray moved(std::move(tray));
    AURORA_TEST_CHECK_STREQ(moved.last_balloon_message(), "msg");
#endif
}

AURORA_TEST_CASE(tray_callback_classifies_version4_events_by_loword_only) {
    using aurora::internal::AURORA_TRAY_ICON_ID;
    using aurora::internal::AURORA_TRAY_NIN_BALLOONUSERCLICK;
    using aurora::internal::AURORA_TRAY_NIN_KEYSELECT;
    using aurora::internal::AURORA_TRAY_NIN_SELECT;
    using aurora::internal::AURORA_TRAY_WM_CONTEXTMENU;
    using aurora::internal::AURORA_TRAY_WM_LBUTTONUP;
    using aurora::internal::AURORA_TRAY_WM_RBUTTONUP;
    using aurora::internal::classify_tray_callback;

    // 版本 4：高字是 uID、低字是事件（实测一次左键连发 POPUPOPEN/DOWN/UP/NIN_SELECT 四条）。
    auto ev = classify_tray_callback(AURORA_TRAY_NIN_SELECT, AURORA_TRAY_ICON_ID, true);
    AURORA_TEST_CHECK_TRUE(ev.activate);
    AURORA_TEST_CHECK_FALSE(ev.context_menu);
    AURORA_TEST_CHECK_TRUE(classify_tray_callback(AURORA_TRAY_NIN_KEYSELECT, AURORA_TRAY_ICON_ID, true).activate);
    AURORA_TEST_CHECK_TRUE(
        classify_tray_callback(AURORA_TRAY_NIN_BALLOONUSERCLICK, AURORA_TRAY_ICON_ID, true).activate);

    // 同一次点击附带的裸鼠标消息必须忽略，否则 on_activate 每次点击触发两遍。
    ev = classify_tray_callback(AURORA_TRAY_WM_LBUTTONUP, AURORA_TRAY_ICON_ID, true);
    AURORA_TEST_CHECK_FALSE(ev.activate);
    AURORA_TEST_CHECK_FALSE(ev.context_menu);

    // 菜单只认 WM_CONTEXTMENU：右键的 WM_RBUTTONUP 是它前一条重复投递。
    ev = classify_tray_callback(AURORA_TRAY_WM_CONTEXTMENU, AURORA_TRAY_ICON_ID, true);
    AURORA_TEST_CHECK_TRUE(ev.context_menu);
    AURORA_TEST_CHECK_FALSE(ev.activate);
    AURORA_TEST_CHECK_FALSE(classify_tray_callback(AURORA_TRAY_WM_RBUTTONUP, AURORA_TRAY_ICON_ID, true).context_menu);

    // 别的图标（uID 不符）不归本库管。
    AURORA_TEST_CHECK_FALSE(classify_tray_callback(AURORA_TRAY_NIN_SELECT, AURORA_TRAY_ICON_ID + 1, true).activate);
}

AURORA_TEST_CASE(tray_callback_classifies_legacy_whole_lparam_and_ignores_noise) {
    using aurora::internal::AURORA_TRAY_WM_CONTEXTMENU;
    using aurora::internal::AURORA_TRAY_WM_LBUTTONDBLCLK;
    using aurora::internal::AURORA_TRAY_WM_LBUTTONUP;
    using aurora::internal::AURORA_TRAY_WM_RBUTTONUP;
    using aurora::internal::classify_tray_callback;

    // NIM_SETVERSION 失败即回落旧编码：整个 lParam 就是鼠标消息，高字恒 0。
    AURORA_TEST_CHECK_TRUE(classify_tray_callback(AURORA_TRAY_WM_LBUTTONUP, 0, false).activate);
    AURORA_TEST_CHECK_TRUE(classify_tray_callback(AURORA_TRAY_WM_LBUTTONDBLCLK, 0, false).activate);
    AURORA_TEST_CHECK_TRUE(classify_tray_callback(AURORA_TRAY_WM_RBUTTONUP, 0, false).context_menu);
    AURORA_TEST_CHECK_TRUE(classify_tray_callback(AURORA_TRAY_WM_CONTEXTMENU, 0, false).context_menu);

    // 无关消息既不激活也不弹菜单（NIN_POPUPOPEN / NIN_BALLOONHIDE / WM_MOUSEMOVE）。
    auto ev = classify_tray_callback(0x0406, 0, false);
    AURORA_TEST_CHECK_FALSE(ev.activate);
    AURORA_TEST_CHECK_FALSE(ev.context_menu);
    ev = classify_tray_callback(0x0200, 0, true);
    AURORA_TEST_CHECK_FALSE(ev.activate);
    AURORA_TEST_CHECK_FALSE(ev.context_menu);
}

}  // namespace aurora::test_cases::utest_system_tray
