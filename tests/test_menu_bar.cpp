// 验证 MenuBar：展开/收起、菜单项触发、分隔符/禁用项、点击交互、序列化。

#include <memory>

#include "aurora/widget/menu_bar.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Menu;
using aurora::MenuBar;
using aurora::MenuItem;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Rect;
using aurora::Size;

namespace {

auto make_menus(int *open_count, int *save_count) -> std::vector<Menu> {
    std::vector<Menu> menus;
    Menu file;
    file.title = "File";
    file.items.emplace_back("Open", [open_count]() -> void {
        if (open_count) {
            ++*open_count;
        }
    });
    file.items.emplace_back("Save", [save_count]() -> void {
        if (save_count) {
            ++*save_count;
        }
    });
    file.items.push_back(MenuItem::separator_item());
    MenuItem disabled{ "Locked", []() -> void {} };
    disabled.enabled = false;
    file.items.push_back(disabled);

    Menu edit;
    edit.title = "Edit";
    edit.items.emplace_back("Copy");
    menus.push_back(std::move(file));
    menus.push_back(std::move(edit));
    return menus;
}

auto layout_bar(std::shared_ptr<MenuBar> const &mb, float w, float h) -> void {
    constexpr BuildContext ctx;
    mb->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = w, .height = h };
    mb->layout(c, ctx);
}

auto press_at(std::shared_ptr<MenuBar> const &mb, float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.local_position = Point{ .x = x, .y = y };
    mb->on_pointer_event(e);
    return e;
}

} // namespace

AURORA_TEST() {
    // ---- 1. 构造与布局 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        AURORA_TEST_CHECK(mb->menu_count() == 2);
        AURORA_TEST_CHECK(!mb->is_open());

        layout_bar(mb, 640.0f, 480.0f);
        // 布局高度 = 菜单栏高度
        AURORA_TEST_CHECK(mb->size().height == mb->bar_height());
        AURORA_TEST_CHECK(mb->size().width == 640.0f);
    }

    // ---- 2. 点击标题展开/再点收起 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        layout_bar(mb, 640.0f, 480.0f);

        auto e = press_at(mb, 10.0f, 14.0f); // File 标题
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(mb->is_open());
        AURORA_TEST_CHECK(mb->open_menu() == 0);

        // 再点同一标题收起
        press_at(mb, 10.0f, 14.0f);
        AURORA_TEST_CHECK(!mb->is_open());
    }

    // ---- 3. 展开后点击菜单项触发回调并收起 ----
    {
        int opened = 0;
        int saved = 0;
        auto mb = std::make_shared<MenuBar>(make_menus(&opened, &saved));
        layout_bar(mb, 640.0f, 480.0f);

        mb->open(0);
        const Rect drop = mb->dropdown_bounds();
        AURORA_TEST_CHECK(drop.size.height > 0.0f);

        // 第一项 "Open"（行高 26，点击第一行中心）
        press_at(mb, drop.origin.x + 20.0f, drop.origin.y + 13.0f);
        AURORA_TEST_CHECK(opened == 1);
        AURORA_TEST_CHECK(!mb->is_open()); // 触发后收起

        // 第二项 "Save"
        mb->open(0);
        press_at(mb, drop.origin.x + 20.0f, drop.origin.y + 26.0f + 13.0f);
        AURORA_TEST_CHECK(saved == 1);
    }

    // ---- 4. 分隔符与禁用项不触发 ----
    {
        int opened = 0;
        int saved = 0;
        auto mb = std::make_shared<MenuBar>(make_menus(&opened, &saved));
        layout_bar(mb, 640.0f, 480.0f);

        mb->open(0);
        const Rect drop = mb->dropdown_bounds();

        // 第三行分隔符
        press_at(mb, drop.origin.x + 20.0f, drop.origin.y + (26.0f * 2) + 13.0f);
        AURORA_TEST_CHECK(opened == 0 && saved == 0);
        AURORA_TEST_CHECK(!mb->is_open()); // 点击后仍收起

        // 第四行禁用项
        mb->open(0);
        press_at(mb, drop.origin.x + 20.0f, drop.origin.y + (26.0f * 3) + 13.0f);
        AURORA_TEST_CHECK(opened == 0 && saved == 0);
    }

    // ---- 5. 下拉外点击收起 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        layout_bar(mb, 640.0f, 480.0f);

        mb->open(0);
        auto e = press_at(mb, 500.0f, 300.0f); // 远离下拉
        AURORA_TEST_CHECK(e.handled);
        AURORA_TEST_CHECK(!mb->is_open());
    }

    // ---- 6. open 越界忽略 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        mb->open(5);
        AURORA_TEST_CHECK(!mb->is_open());
        mb->open(-1);
        AURORA_TEST_CHECK(!mb->is_open());
    }

    // ---- 7. add_menu 与序列化 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        Menu help;
        help.title = "Help";
        mb->add_menu(std::move(help));
        AURORA_TEST_CHECK(mb->menu_count() == 3);

        aurora::Json props;
        mb->serialize_props(props);
        AURORA_TEST_CHECK(props["menu_titles"].size() == 3);
        AURORA_TEST_CHECK(props["menu_titles"][2].get<std::string>() == "Help");
    }

    // ---- 8. 命中测试：栏内命中、下拉展开时命中、栏外不命中 ----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        layout_bar(mb, 640.0f, 480.0f);

        BuildContext ctx;
        constexpr Rect bounds{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                               .size = Size{ .width = 640.0f, .height = 28.0f } };

        AURORA_TEST_CHECK(mb->hit_test(Point{ .x = 10.0f, .y = 14.0f }, bounds, ctx) != nullptr);
        // 收起时下方区域不命中
        AURORA_TEST_CHECK(mb->hit_test(Point{ .x = 10.0f, .y = 100.0f }, bounds, ctx) == nullptr);
        // 展开时下拉区域命中
        mb->open(0);
        const Rect drop = mb->dropdown_bounds();
        AURORA_TEST_CHECK(mb->hit_test(Point{ .x = drop.origin.x + 5.0f, .y = drop.origin.y + 5.0f }, bounds, ctx) !=
                          nullptr);
    }

    // ---- 9. 无头渲染不崩溃（展开态含阴影/下拉）----
    {
        auto mb = std::make_shared<MenuBar>(make_menus(nullptr, nullptr));
        layout_bar(mb, 320.0f, 240.0f);
        mb->open(0);

        aurora::Painter p;
        p.begin(320, 240);
        BuildContext ctx;
        mb->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 320.0f, .height = 28.0f } },
                  ctx);
        AURORA_TEST_CHECK(p.width() == 320);
    }
}
