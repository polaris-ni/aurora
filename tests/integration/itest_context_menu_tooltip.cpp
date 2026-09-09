/// 测试类型: integration
/// 目标单元: include/aurora/modifier/modifier_input.h + include/aurora/app/menu.h
/// 测试说明: 输入修饰节点集成——MenuItem 构造与子菜单、TooltipNode 延迟/可见性计时、
/// ContextMenuNode 打开/关闭/位置、Modifier 工厂组合、Widget 右键拦截与 tick 驱动
/// Tooltip 延迟、负延迟钳制

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/event/event.h"
#include "aurora/modifier/modifier.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_context_menu_tooltip {

AURORA_TEST_CASE(menu_item_construction_and_submenu) {
    MenuItem item{"Open", []() -> void {}};
    AURORA_TEST_CHECK_STREQ(item.label, "Open");
    AURORA_TEST_CHECK_FALSE(item.separator);
    AURORA_TEST_CHECK_FALSE(item.is_submenu());
    AURORA_TEST_CHECK_TRUE(item.enabled);

    const MenuItem sep = MenuItem::separator_item();
    AURORA_TEST_CHECK_TRUE(sep.separator);

    MenuItem parent{"File"};
    parent.children.emplace_back("New");
    AURORA_TEST_CHECK_TRUE(parent.is_submenu());
}

AURORA_TEST_CASE(tooltip_node_visibility_follows_delay) {
    TooltipNode tt{"Hello Tooltip", 100.0F};
    AURORA_TEST_CHECK_STREQ(tt.text(), "Hello Tooltip");
    AURORA_TEST_CHECK_NEAR(tt.delay_ms(), 100.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(tt.is_visible());

    const auto now = std::chrono::steady_clock::now();

    // 未 hover 时 tick 不触发。
    tt.tick(now + std::chrono::milliseconds(200));
    AURORA_TEST_CHECK_FALSE(tt.is_visible());

    // hover 开始后未达阈值。
    tt.hover_start(now);
    tt.tick(now + std::chrono::milliseconds(50));
    AURORA_TEST_CHECK_FALSE(tt.is_visible());

    // 达到阈值后可见（幂等保持）。
    tt.tick(now + std::chrono::milliseconds(150));
    AURORA_TEST_CHECK_TRUE(tt.is_visible());

    // hover 结束后重置。
    tt.hover_end();
    AURORA_TEST_CHECK_FALSE(tt.is_visible());
}

AURORA_TEST_CASE(tooltip_node_negative_delay_clamps_to_zero) {
    TooltipNode tt{"Instant", -10.0F};
    AURORA_TEST_CHECK_NEAR(tt.delay_ms(), 0.0F, 1e-4F);

    const auto now = std::chrono::steady_clock::now();
    tt.hover_start(now);
    tt.tick(now);  // 0ms 延迟，立即触发
    AURORA_TEST_CHECK_TRUE(tt.is_visible());
}

AURORA_TEST_CASE(context_menu_node_open_close_and_position) {
    std::vector items = {MenuItem{"Copy"}, MenuItem{"Paste"}};
    ContextMenuNode cm{items};
    AURORA_TEST_CHECK_FALSE(cm.is_open());
    AURORA_TEST_CHECK_EQ(cm.items().size(), 2U);

    cm.open_at(Point{.x = 100.0F, .y = 200.0F});
    AURORA_TEST_CHECK_TRUE(cm.is_open());
    AURORA_TEST_CHECK_NEAR(cm.position().x, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cm.position().y, 200.0F, 1e-4F);

    cm.close();
    AURORA_TEST_CHECK_FALSE(cm.is_open());
}

AURORA_TEST_CASE(modifier_factories_compose_input_nodes) {
    const auto mod = Modifier{}.tooltip("Tip text", 300.0F).context_menu(std::vector{MenuItem{"Action"}});

    AURORA_TEST_CHECK_EQ(mod.nodes().size(), 2U);
    AURORA_TEST_CHECK_TRUE(mod.has_context_menu());

    // active 视图初始为空（未 hover / 未打开）。
    AURORA_TEST_CHECK_TRUE(mod.active_tooltip().empty());
    AURORA_TEST_CHECK_TRUE(mod.active_context_menu_items().empty());
}

AURORA_TEST_CASE(modifier_tooltip_tick_drives_active_text) {
    const auto mod = Modifier{}.tooltip("Delayed", 50.0F);
    const auto now = std::chrono::steady_clock::now();

    mod.tooltip_hover_start(now);
    mod.tick_tooltip(now + std::chrono::milliseconds(30));
    AURORA_TEST_CHECK_TRUE(mod.active_tooltip().empty());

    mod.tick_tooltip(now + std::chrono::milliseconds(60));
    AURORA_TEST_CHECK_STREQ(mod.active_tooltip(), "Delayed");

    mod.tooltip_hover_end();
    AURORA_TEST_CHECK_TRUE(mod.active_tooltip().empty());
}

AURORA_TEST_CASE(modifier_context_menu_open_close) {
    const auto mod = Modifier{}.context_menu(std::vector{MenuItem{"Edit"}, MenuItem{"Delete"}});
    AURORA_TEST_CHECK_TRUE(mod.has_context_menu());

    mod.open_context_menu(Point{.x = 50.0F, .y = 75.0F});
    const auto items = mod.active_context_menu_items();
    AURORA_TEST_CHECK_EQ(items.size(), 2U);
    AURORA_TEST_CHECK_STREQ(items.at(0).label, "Edit");
    const Point pos = mod.active_context_menu_position();
    AURORA_TEST_CHECK_NEAR(pos.x, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pos.y, 75.0F, 1e-4F);

    mod.close_context_menu();
    AURORA_TEST_CHECK_TRUE(mod.active_context_menu_items().empty());
}

AURORA_TEST_CASE(widget_right_click_opens_context_menu) {
    auto t = std::make_shared<Text>("Target");
    t->modifier.set(Modifier{}.context_menu(std::vector{MenuItem{"Cut"}}));

    const BuildContext ctx;
    t->mount(ctx);

    // 右键按下应被消费并打开上下文菜单。
    MouseEvent e;
    e.button = MouseButton::Right;
    e.action = MouseAction::Press;
    e.position = Point{.x = 10.0F, .y = 10.0F};
    t->on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(e.is_handled);

    const auto items = t->modifier.get().active_context_menu_items();
    AURORA_TEST_CHECK_EQ(items.size(), 1U);
    AURORA_TEST_CHECK_STREQ(items.at(0).label, "Cut");

    // 左键按下不应触发上下文菜单。
    t->modifier.get().close_context_menu();
    MouseEvent e2;
    e2.button = MouseButton::Left;
    e2.action = MouseAction::Press;
    e2.position = Point{.x = 10.0F, .y = 10.0F};
    t->on_pointer_event(e2);
    AURORA_TEST_CHECK_TRUE(t->modifier.get().active_context_menu_items().empty());
}

AURORA_TEST_CASE(widget_tick_drives_tooltip_delay) {
    auto t = std::make_shared<Text>("Hover me");
    t->modifier.set(Modifier{}.tooltip("Widget Tip", 80.0F));

    const BuildContext ctx;
    t->mount(ctx);

    const auto now = std::chrono::steady_clock::now();
    t->modifier.get().tooltip_hover_start(now);

    // 未达阈值。
    t->tick(now + std::chrono::milliseconds(40));
    AURORA_TEST_CHECK_TRUE(t->modifier.get().active_tooltip().empty());

    // 达到阈值。
    t->tick(now + std::chrono::milliseconds(100));
    AURORA_TEST_CHECK_STREQ(t->modifier.get().active_tooltip(), "Widget Tip");
}

AURORA_TEST_CASE(context_menu_submenu_items_preserved) {
    MenuItem file{"File"};
    file.children.emplace_back("New");
    file.children.emplace_back("Open");
    file.children.push_back(MenuItem::separator_item());
    file.children.emplace_back("Exit");

    auto mod = Modifier{}.context_menu(std::vector{file, MenuItem{"Help"}});
    mod.open_context_menu(Point{.x = 0.0F, .y = 0.0F});

    const auto items = mod.active_context_menu_items();
    AURORA_TEST_CHECK_EQ(items.size(), 2U);
    AURORA_TEST_CHECK_TRUE(items.at(0).is_submenu());
    AURORA_TEST_CHECK_EQ(items.at(0).children.size(), 4U);
    AURORA_TEST_CHECK_TRUE(items.at(0).children[2].separator);
}

}  // namespace aurora::test_cases::itest_context_menu_tooltip
