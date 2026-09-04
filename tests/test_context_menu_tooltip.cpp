// 验证新修饰节点：Tooltip（悬停延迟提示）与 ContextMenu（右键菜单）。
// 检查：TooltipNode 计时/可见性、ContextMenuNode 打开/关闭、Modifier 工厂方法、
//       Widget 右键事件拦截、tick_gestures 驱动 Tooltip 延迟。
// ── API 覆盖映射 ─────────────────────────────
// modifier/modifier_input.h(Input 修饰家族：Tooltip/ContextMenu/Clickable/hover 等)。

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "aurora/app/menu.h"
#include "aurora/event/event.h"
#include "aurora/modifier/modifier.h"
#include "aurora/widget/text.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::ContextMenuNode;
using aurora::MenuItem;
using aurora::Modifier;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Text;
using aurora::TooltipNode;

AURORA_TEST() {
    // ---- 1. MenuItem 基本构造 ----
    {
        MenuItem item{"Open", []() -> void {}};
        AURORA_TEST_CHECK(item.label == "Open");
        AURORA_TEST_CHECK(!item.separator);
        AURORA_TEST_CHECK(!item.is_submenu());
        AURORA_TEST_CHECK(item.enabled);

        auto sep = MenuItem::separator_item();
        AURORA_TEST_CHECK(sep.separator);

        MenuItem parent{"File"};
        parent.children.emplace_back("New");
        AURORA_TEST_CHECK(parent.is_submenu());
    }

    // ---- 2. TooltipNode 计时逻辑 ----
    {
        TooltipNode tt{"Hello Tooltip", 100.0F};
        AURORA_TEST_CHECK(tt.text() == "Hello Tooltip");
        AURORA_TEST_CHECK(tt.delay_ms() == 100.0F);
        AURORA_TEST_CHECK(!tt.is_visible());

        // 未 hover 时 tick 不触发
        auto now = std::chrono::steady_clock::now();
        tt.tick(now + std::chrono::milliseconds(200));
        AURORA_TEST_CHECK(!tt.is_visible());

        // hover 开始后未达阈值
        tt.hover_start(now);
        tt.tick(now + std::chrono::milliseconds(50));
        AURORA_TEST_CHECK(!tt.is_visible());

        // 达到阈值后可见
        tt.tick(now + std::chrono::milliseconds(150));
        AURORA_TEST_CHECK(tt.is_visible());

        // hover 结束后重置
        tt.hover_end();
        AURORA_TEST_CHECK(!tt.is_visible());
    }

    // ---- 3. ContextMenuNode 打开/关闭 ----
    {
        std::vector items = {MenuItem{"Copy"}, MenuItem{"Paste"}};
        ContextMenuNode cm{items};
        AURORA_TEST_CHECK(!cm.is_open());
        AURORA_TEST_CHECK(cm.items().size() == 2);

        cm.open_at(Point{.x = 100.0F, .y = 200.0F});
        AURORA_TEST_CHECK(cm.is_open());
        AURORA_TEST_CHECK(cm.position().x == 100.0F);
        AURORA_TEST_CHECK(cm.position().y == 200.0F);

        cm.close();
        AURORA_TEST_CHECK(!cm.is_open());
    }

    // ---- 4. Modifier 工厂方法 ----
    {
        auto mod = Modifier{}.tooltip("Tip text", 300.0F).context_menu({MenuItem{"Action"}});

        AURORA_TEST_CHECK(mod.nodes().size() == 2);
        AURORA_TEST_CHECK(mod.has_context_menu());

        // active_tooltip 初始为空（未 hover）
        AURORA_TEST_CHECK(mod.active_tooltip().empty());

        // active_context_menu_items 初始为空（未打开）
        AURORA_TEST_CHECK(mod.active_context_menu_items().empty());
    }

    // ---- 5. Modifier Tooltip 计时驱动 ----
    {
        auto mod = Modifier{}.tooltip("Delayed", 50.0F);
        auto now = std::chrono::steady_clock::now();

        mod.tooltip_hover_start(now);
        mod.tick_tooltip(now + std::chrono::milliseconds(30));
        AURORA_TEST_CHECK(mod.active_tooltip().empty());

        mod.tick_tooltip(now + std::chrono::milliseconds(60));
        AURORA_TEST_CHECK(mod.active_tooltip() == "Delayed");

        mod.tooltip_hover_end();
        AURORA_TEST_CHECK(mod.active_tooltip().empty());
    }

    // ---- 6. Modifier ContextMenu 打开/关闭 ----
    {
        auto mod = Modifier{}.context_menu({MenuItem{"Edit"}, MenuItem{"Delete"}});
        AURORA_TEST_CHECK(!mod.has_context_menu() == false);  // has_context_menu == true
        AURORA_TEST_CHECK(mod.has_context_menu());

        mod.open_context_menu(Point{.x = 50.0F, .y = 75.0F});
        auto items = mod.active_context_menu_items();
        AURORA_TEST_CHECK(items.size() == 2);
        AURORA_TEST_CHECK(items.at(0).label == "Edit");
        auto pos = mod.active_context_menu_position();
        AURORA_TEST_CHECK(pos.x == 50.0F);
        AURORA_TEST_CHECK(pos.y == 75.0F);

        mod.close_context_menu();
        AURORA_TEST_CHECK(mod.active_context_menu_items().empty());
    }

    // ---- 7. Widget 右键事件拦截 ----
    {
        auto t = std::make_shared<Text>();
        t->modifier.set(Modifier{}.context_menu({MenuItem{"Cut"}}));

        BuildContext ctx;
        t->mount(ctx);

        // 右键按下应被消费
        MouseEvent e;
        e.button = MouseButton::Right;
        e.action = MouseAction::Press;
        e.position = Point{.x = 10.0F, .y = 10.0F};
        t->on_pointer_event(e);
        AURORA_TEST_CHECK(e.is_handled_);

        // 上下文菜单应已打开
        auto items = t->modifier.get().active_context_menu_items();
        AURORA_TEST_CHECK(items.size() == 1);
        AURORA_TEST_CHECK(items.at(0).label == "Cut");

        // 左键按下不应触发上下文菜单
        t->modifier.get().close_context_menu();
        MouseEvent e2;
        e2.button = MouseButton::Left;
        e2.action = MouseAction::Press;
        e2.position = Point{.x = 10.0F, .y = 10.0F};
        t->on_pointer_event(e2);
        AURORA_TEST_CHECK(t->modifier.get().active_context_menu_items().empty());
    }

    // ---- 8. Widget tick 驱动 Tooltip ----
    {
        auto t = std::make_shared<Text>();
        t->modifier.set(Modifier{}.tooltip("Widget Tip", 80.0F));

        BuildContext ctx;
        t->mount(ctx);

        auto now = std::chrono::steady_clock::now();
        t->modifier.get().tooltip_hover_start(now);

        // 未达阈值
        t->tick(now + std::chrono::milliseconds(40));
        AURORA_TEST_CHECK(t->modifier.get().active_tooltip().empty());

        // 达到阈值
        t->tick(now + std::chrono::milliseconds(100));
        AURORA_TEST_CHECK(t->modifier.get().active_tooltip() == "Widget Tip");
    }

    // ---- 9. Tooltip 负值延迟降级为 0 ----
    {
        TooltipNode tt{"Instant", -10.0F};
        AURORA_TEST_CHECK(tt.delay_ms() == 0.0F);

        auto now = std::chrono::steady_clock::now();
        tt.hover_start(now);
        tt.tick(now);  // 0ms 延迟，立即触发
        AURORA_TEST_CHECK(tt.is_visible());
    }

    // ---- 10. ContextMenu 多菜单项含子菜单 ----
    {
        MenuItem file{"File"};
        file.children.emplace_back("New");
        file.children.emplace_back("Open");
        file.children.push_back(MenuItem::separator_item());
        file.children.emplace_back("Exit");

        auto mod = Modifier{}.context_menu({file, MenuItem{"Help"}});
        mod.open_context_menu(Point{.x = 0.0F, .y = 0.0F});

        auto items = mod.active_context_menu_items();
        AURORA_TEST_CHECK(items.size() == 2);
        AURORA_TEST_CHECK(items.at(0).is_submenu());
        AURORA_TEST_CHECK(items.at(0).children.size() == 4);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(items.at(0).children[2].separator);
    }
}