/// 测试类型: integration
/// 目标单元: include/aurora/event/focus.h
/// 测试说明: 键盘导航集成——Tab 序前进/后退循环、方向键按 focus_bounds 几何导航
/// （水平/垂直）、activate() 触发 on_click、无可聚焦候选时安全返回 false

#include <memory>
#include <utility>

#include "aurora/event/focus.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_keyboard_nav {

AURORA_TEST_CASE(tab_cycle_forward_and_backward) {
    Button btn1{"A"};
    btn1.set_focusable(true);
    btn1.set_tab_index(0);
    btn1.set_focus_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});

    Button btn2{"B"};
    btn2.set_focusable(true);
    btn2.set_tab_index(1);
    btn2.set_focus_bounds(Rect{.origin = Point{.x = 0.0F, .y = 50.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});

    // 容器不参与焦点序（Widget 默认 focusable=true，须显式关闭）。
    Column col{Node{std::move(btn1)}, Node{std::move(btn2)}};
    col.set_focusable(false);

    FocusManager fm;
    fm.set_root(&col);

    // 初始无焦点。
    AURORA_TEST_CHECK_NULL(fm.focused());

    // Tab 前进 → 第一个。
    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_REQUIRE_NOT_NULL(fm.focused());
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);

    // Tab 前进 → 第二个。
    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 1);

    // Tab 前进 → 循环回第一个。
    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);

    // Shift+Tab 后退 → 第二个。
    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Backward));
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 1);
}

AURORA_TEST_CASE(directional_nav_moves_by_geometry_horizontally) {
    // 3x1 水平排列：A(0,0) B(110,0) C(220,0)。
    auto make_btn = [](const char* label, int tab, float x) -> Node {
        Button b{label};
        b.set_focusable(true);
        b.set_tab_index(tab);
        b.set_focus_bounds(Rect{.origin = Point{.x = x, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});
        return Node{std::move(b)};
    };

    Column col{make_btn("A", 0, 0.0F), make_btn("B", 1, 110.0F), make_btn("C", 2, 220.0F)};
    col.set_focusable(false);  // 容器不参与焦点序（Widget 默认 focusable=true）

    FocusManager fm;
    fm.set_root(&col);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Forward));  // → A
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Right));  // → B
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 1);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Right));  // → C
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 2);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Left));  // → B
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 1);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Left));  // → A
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);
}

AURORA_TEST_CASE(directional_nav_moves_by_geometry_vertically) {
    // 垂直排列：Top(0,0) Bottom(0,60)。
    Button btn_top{"Top"};
    btn_top.set_focusable(true);
    btn_top.set_tab_index(0);
    btn_top.set_focus_bounds(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});

    Button btn_bot{"Bottom"};
    btn_bot.set_focusable(true);
    btn_bot.set_tab_index(1);
    btn_bot.set_focus_bounds(
        Rect{.origin = Point{.x = 0.0F, .y = 60.0F}, .size = Size{.width = 100.0F, .height = 40.0F}});

    Column col{Node{std::move(btn_top)}, Node{std::move(btn_bot)}};
    col.set_focusable(false);  // 容器不参与焦点序（Widget 默认 focusable=true）

    FocusManager fm;
    fm.set_root(&col);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Forward));  // → Top
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Down));  // → Bottom
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 1);

    AURORA_TEST_CHECK_TRUE(fm.move_focus(FocusDirection::Up));  // → Top
    AURORA_TEST_CHECK_EQ(fm.focused()->tab_index(), 0);
}

AURORA_TEST_CASE(activate_triggers_on_click) {
    bool clicked = false;
    Button btn{"Click"};
    btn.set_focusable(true);
    btn.set_on_click([&clicked]() -> void { clicked = true; });

    // activate() 应触发 on_click（Enter/Space 激活路径的直达入口）。
    btn.activate();
    AURORA_TEST_CHECK_TRUE(clicked);
}

AURORA_TEST_CASE(no_focusable_candidates_returns_false) {
    Text txt{"Not focusable"};
    txt.set_focusable(false);  // 显式设为不可聚焦
    Column col{Node{std::move(txt)}};
    col.set_focusable(false);  // 容器同样不参与（Widget 默认 focusable=true）

    FocusManager fm;
    fm.set_root(&col);

    AURORA_TEST_CHECK_FALSE(fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK_NULL(fm.focused());
}

}  // namespace aurora::test_cases::itest_keyboard_nav
