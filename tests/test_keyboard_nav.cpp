// test_keyboard_nav.cpp — 键盘导航增强测试。

#include "aurora/aurora.h"
#include "aurora/event/focus.h"
#include "test_harness.h"

using aurora::Button;
using aurora::ButtonProps;
using aurora::Column;
using aurora::ColumnProps;
using aurora::FocusDirection;
using aurora::FocusManager;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Text;

// ---------- Tab 循环焦点 ----------

static void test_tab_cycle() {
    // 构建两个可聚焦控件
    auto btn1 = Button(ButtonProps{.label = "A"});
    btn1.set_focusable(true);
    btn1.set_tab_index(0);
    btn1.set_focus_bounds(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 40}});

    auto btn2 = Button(ButtonProps{.label = "B"});
    btn2.set_focusable(true);
    btn2.set_tab_index(1);
    btn2.set_focus_bounds(Rect{.origin = Point{.x = 0, .y = 50}, .size = Size{.width = 100, .height = 40}});

    // 用 Column 包裹（容器不参与焦点序）
    auto col = Column(ColumnProps{.children = {std::move(btn1), std::move(btn2)}});
    col.set_focusable(false);

    FocusManager fm;
    fm.set_root(&col);

    // 初始无焦点
    AURORA_TEST_CHECK(fm.focused() == nullptr);

    // Tab 前进 → 第一个
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused() != nullptr);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);

    // Tab 前进 → 第二个
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 1);

    // Tab 前进 → 循环回第一个
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);

    // Shift+Tab 后退 → 第二个
    fm.move_focus(FocusDirection::Backward);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 1);
}

// ---------- 方向键几何导航 ----------

static void test_directional_nav() {
    // 3x1 水平排列：A(0,0) B(110,0) C(220,0)
    auto btn_a = Button(ButtonProps{.label = "A"});
    btn_a.set_focusable(true);
    btn_a.set_tab_index(0);
    btn_a.set_focus_bounds(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 40}});

    auto btn_b = Button(ButtonProps{.label = "B"});
    btn_b.set_focusable(true);
    btn_b.set_tab_index(1);
    btn_b.set_focus_bounds(Rect{.origin = Point{.x = 110, .y = 0}, .size = Size{.width = 100, .height = 40}});

    auto btn_c = Button(ButtonProps{.label = "C"});
    btn_c.set_focusable(true);
    btn_c.set_tab_index(2);
    btn_c.set_focus_bounds(Rect{.origin = Point{.x = 220, .y = 0}, .size = Size{.width = 100, .height = 40}});

    auto col = Column(ColumnProps{.children = {std::move(btn_a), std::move(btn_b), std::move(btn_c)}});
    col.set_focusable(false);

    FocusManager fm;
    fm.set_root(&col);

    // 聚焦 A
    fm.move_focus(FocusDirection::Forward);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);

    // Right → B
    fm.move_focus(FocusDirection::Right);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 1);

    // Right → C
    fm.move_focus(FocusDirection::Right);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 2);

    // Left → B
    fm.move_focus(FocusDirection::Left);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 1);

    // Left → A
    fm.move_focus(FocusDirection::Left);
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);
}

// ---------- 垂直方向导航 ----------

static void test_vertical_nav() {
    // 垂直排列：Top(0,0) Bottom(0,60)
    auto btn_top = Button(ButtonProps{.label = "Top"});
    btn_top.set_focusable(true);
    btn_top.set_tab_index(0);
    btn_top.set_focus_bounds(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 40}});

    auto btn_bot = Button(ButtonProps{.label = "Bottom"});
    btn_bot.set_focusable(true);
    btn_bot.set_tab_index(1);
    btn_bot.set_focus_bounds(Rect{.origin = Point{.x = 0, .y = 60}, .size = Size{.width = 100, .height = 40}});

    auto col = Column(ColumnProps{.children = {std::move(btn_top), std::move(btn_bot)}});
    col.set_focusable(false);

    FocusManager fm;
    fm.set_root(&col);

    fm.move_focus(FocusDirection::Forward);  // → Top
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);

    fm.move_focus(FocusDirection::Down);  // → Bottom
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 1);

    fm.move_focus(FocusDirection::Up);  // → Top
    AURORA_TEST_CHECK(fm.focused()->tab_index() == 0);
}

// ---------- Enter/Space 激活 ----------

static void test_activate() {
    bool clicked = false;
    auto btn = Button(ButtonProps{.label = "Click"});
    btn.set_focusable(true);
    btn.set_on_click([&]() -> void { clicked = true; });

    // activate() 应触发 on_click
    btn.activate();
    AURORA_TEST_CHECK(clicked);
}

// ---------- 无候选时不崩溃 ----------

static void test_no_candidates() {
    auto txt = Text("Not focusable");
    txt.set_focusable(false);  // 显式设为不可聚焦
    auto col = Column(ColumnProps{.children = {std::move(txt)}});
    col.set_focusable(false);

    FocusManager fm;
    fm.set_root(&col);

    AURORA_TEST_CHECK(!fm.move_focus(FocusDirection::Forward));
    AURORA_TEST_CHECK(fm.focused() == nullptr);
}

AURORA_TEST() {
    test_tab_cycle();
    test_directional_nav();
    test_vertical_nav();
    test_activate();
    test_no_candidates();
}
