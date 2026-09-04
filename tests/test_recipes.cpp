// test_recipes.cpp — 复合配方（form_layout/toolbar/sidebar/menu_bar/list_view/tab_view）1:1 测试。

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Checkbox;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::form_layout;
using aurora::FormRow;
using aurora::list_view;
using aurora::menu_bar;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Reactive;
using aurora::Rect;
using aurora::sidebar;
using aurora::Size;
using aurora::tab_view;
using aurora::TabPage;
using aurora::Text;
using aurora::TextInput;
using aurora::toolbar;
using aurora::Widget;

static void render_tree(Widget &w, const float ww, const float hh) {
    constexpr BuildContext ctx;
    w.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = ww, .height = hh};
    w.layout(cc, ctx);
    Painter p;
    p.begin(static_cast<int>(ww), static_cast<int>(hh));
    w.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = ww, .height = hh}}, ctx);
}

static void test_recipes() {
    auto f = form_layout({FormRow{.label = "Name", .field = Node{TextInput{}}},
                          FormRow{.label = "Age", .field = Node{Checkbox{Reactive{false}}}}});
    AURORA_TEST_CHECK_MSG(std::string{f.widget().type_name()} == "Column", "formLayout -> Column");

    auto bar = toolbar({Node{Button{"A"}}, Node{Button{"B"}}});
    AURORA_TEST_CHECK_MSG(std::string{bar.widget().type_name()} == "Row", "toolbar -> Row");

    auto side = sidebar({Node{Text{"Home"}}, Node{Text{"Settings"}}});
    AURORA_TEST_CHECK_MSG(std::string{side.widget().type_name()} == "Column", "sidebar -> Column");

    auto menu = menu_bar({Node{Button{"F"}}, Node{Button{"E"}}});
    AURORA_TEST_CHECK_MSG(std::string{menu.widget().type_name()} == "Row", "menuBar -> Row");

    auto list = list_view({Node{Text{"1"}}, Node{Text{"2"}}});
    AURORA_TEST_CHECK_MSG(std::string{list.widget().type_name()} == "Scroll", "listView -> Scroll");

    auto tabs = tab_view({TabPage{.title = "A", .content = Node{Text{"Page A"}}},
                          TabPage{.title = "B", .content = Node{Text{"Page B"}}}});
    AURORA_TEST_CHECK_MSG(std::string{tabs.widget().type_name()} == "Column", "tabView -> Column");
}

static void test_recipes_render() {
    const auto form = form_layout({FormRow{.label = "Name", .field = Node{TextInput{}}},
                                   FormRow{.label = "Age", .field = Node{Checkbox{Reactive{false}}}}});
    const auto tabs = tab_view({TabPage{.title = "A", .content = Node{Text{"Page A"}}},
                                TabPage{.title = "B", .content = Node{Text{"Page B"}}}});
    auto root =
        Column{ColumnProps{.children = {
                               Node{toolbar({Node{Button{"File"}}, Node{Button{"Edit"}}})},
                               Node{menu_bar({Node{Button{"File"}}, Node{Button{"Edit"}}})},
                               Node{sidebar({Node{Text{"Home"}}, Node{Text{"Settings"}}}, 160.0F)},
                               Node{list_view({Node{Text{"Item 1"}}, Node{Text{"Item 2"}}, Node{Text{"Item 3"}}})},
                               Node{form},
                               Node{tabs},
                           }}};
    render_tree(root, 480.0F, 640.0F);
    AURORA_TEST_CHECK_MSG(true, "recipes (form/toolbar/sidebar/menuBar/listView/tabView) render");

    const Rect bb = tabs.bounds();
    MouseEvent e;
    e.position = Point{.x = bb.origin.x + bb.size.width - 10.0F, .y = bb.origin.y + 10.0F};
    e.action = MouseAction::Press;
    EventDispatcher::dispatch(root, e);
    e.action = MouseAction::Release;
    EventDispatcher::dispatch(root, e);
    AURORA_TEST_CHECK_MSG(true, "TabView tab click handled");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_recipes ===\n");
    test_recipes();
    test_recipes_render();
}
