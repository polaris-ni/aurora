/// 测试类型: unit
/// 目标单元: include/aurora/event/dispatcher.h
/// 测试说明: 命中测试最深目标、鼠标冒泡与 stop-on-handled、本地坐标写入与 Press
/// 焦点转移/空白清焦、指针捕获越界续发、悬停进出 diff、悬停光标解析
/// （修饰链 > 虚钩子 > Clickable 缺省，变化才下发）、键盘
/// Tab/激活快捷键与焦点路由、滚轮/文本/文件拖放路由、TouchDispatcher 按指针 id 捕获与合成鼠标事件

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/event/keycode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dispatcher {

namespace {

/// 固定尺寸叶控件：记录收到的指针/键盘/滚轮/文本/拖放事件，可配置是否消费。
class TestBox final : public LeafWidget {
  public:
    float box_width = 40.0F;
    float box_height = 40.0F;
    bool consume_pointer = false;
    bool consume_keys = true;
    bool consume_drop = false;

    int press_count = 0;
    int release_count = 0;
    int move_count = 0;
    int key_count = 0;
    int hover_changes = 0;
    int activations = 0;
    int scroll_count = 0;
    int text_count = 0;
    int composition_count = 0;
    int drop_count = 0;
    Point last_local{};
    std::optional<int> last_pointer_id;
    std::optional<CursorShape> cursor_hook;  // 非 nullopt 时作为 cursor_shape() 虚钩子返回

    using Widget::on_pointer_event;  // 保持基类 TouchEvent 重载可见

    auto type_name() const -> const char* override { return "TestBox"; }

    auto on_layout(const Constraints& c, [[maybe_unused]] const BuildContext& ctx) -> Size override {
        size_ = c.constrain(Size{.width = box_width, .height = box_height});
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter& p, [[maybe_unused]] const Rect& bounds,
                  [[maybe_unused]] const BuildContext& ctx) -> void override {}

    auto on_pointer_event(MouseEvent& e) -> void override {
        switch (e.action) {
            case MouseAction::Press:
                ++press_count;
                break;
            case MouseAction::Release:
                ++release_count;
                break;
            case MouseAction::Move:
                ++move_count;
                break;
        }
        last_local = e.local_position;
        last_pointer_id = e.pointer_id;
        if (consume_pointer) {
            e.is_handled = true;
        }
    }

    auto on_key_event(KeyEvent& e) -> void override {
        ++key_count;
        if (consume_keys) {
            e.is_handled = true;
        }
    }

    auto activate() -> void override { ++activations; }

    auto on_hover_change(bool entered) -> void override {
        ++hover_changes;
        Widget::on_hover_change(entered);
    }

    auto on_scroll(ScrollEvent& e) -> void override {
        ++scroll_count;
        Widget::on_scroll(e);  // 默认消费
    }

    auto on_text_input(TextInputEvent& e) -> void override {
        ++text_count;
        Widget::on_text_input(e);  // 默认消费
    }

    auto on_text_composition(TextCompositionEvent& e) -> void override {
        ++composition_count;
        Widget::on_text_composition(e);  // 默认消费
    }

    auto on_file_drop(FileDropEvent& e) -> void override {
        ++drop_count;
        if (consume_drop) {
            e.is_handled = true;
        }
    }

    [[nodiscard]] auto cursor_shape() const -> std::optional<CursorShape> override { return cursor_hook; }
};

/// 水平排列容器：子控件依次从左往右铺，各自取固定期望尺寸。
class TestRow final : public Container {
  public:
    bool consume_pointer = false;
    int pointer_events = 0;
    int scroll_count = 0;
    int text_count = 0;
    int composition_count = 0;
    Point last_local{};

    using Widget::on_pointer_event;

    auto type_name() const -> const char* override { return "TestRow"; }

    auto on_layout(const Constraints& c, const BuildContext& ctx) -> Size override {
        float x = 0.0F;
        float max_height = 0.0F;
        for (Node& ch : children_) {
            const Size cs = ch.widget().layout(Constraints{.min = Size{}, .max = c.max}, ctx);
            ch.set_bounds(Rect{.origin = Point{.x = x, .y = 0.0F}, .size = cs});
            x += cs.width;
            max_height = std::max(max_height, cs.height);
        }
        size_ = c.constrain(Size{.width = x, .height = max_height});
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter& p, [[maybe_unused]] const Rect& bounds,
                  [[maybe_unused]] const BuildContext& ctx) -> void override {}

    auto on_pointer_event(MouseEvent& e) -> void override {
        ++pointer_events;
        last_local = e.local_position;
        if (consume_pointer) {
            e.is_handled = true;
        }
    }

    auto on_scroll(ScrollEvent& e) -> void override {
        ++scroll_count;
        Widget::on_scroll(e);
    }

    auto on_text_input(TextInputEvent& e) -> void override {
        ++text_count;
        Widget::on_text_input(e);
    }

    auto on_text_composition(TextCompositionEvent& e) -> void override {
        ++composition_count;
        Widget::on_text_composition(e);
    }
};

struct Tree {
    std::shared_ptr<TestRow> row;
    std::shared_ptr<TestBox> box1;
    std::shared_ptr<TestBox> box2;
};

/// 两盒横排：box1 占 (0,0,40,40)，box2 占 (40,0,40,40)，根 80x40。根不可聚焦，
/// 使键盘候选集恰为 [box1, box2]。
auto make_tree() -> Tree {
    auto row = std::make_shared<TestRow>();
    row->set_focusable(false);
    auto box1 = std::make_shared<TestBox>();
    auto box2 = std::make_shared<TestBox>();
    row->add(Node{box1});
    row->add(Node{box2});
    row->layout(Constraints{}, BuildContext{});
    return Tree{.row = row, .box1 = box1, .box2 = box2};
}

}  // namespace

AURORA_TEST_CASE(hit_test_finds_deepest_and_misses_blank) {
    const auto tree = make_tree();

    const auto* left = EventDispatcher::hit_test(*tree.row, Point{.x = 20.0F, .y = 20.0F});
    AURORA_TEST_CHECK(left == tree.box1.get());

    const auto* right = EventDispatcher::hit_test(*tree.row, Point{.x = 60.0F, .y = 20.0F});
    AURORA_TEST_CHECK(right == tree.box2.get());

    // 根矩形外的空白：无命中
    AURORA_TEST_CHECK(EventDispatcher::hit_test(*tree.row, Point{.x = 500.0F, .y = 500.0F}) == nullptr);
}

AURORA_TEST_CASE(mouse_press_bubbles_deepest_to_root_until_handled) {
    auto tree = make_tree();

    // 静态便捷入口：未消费 → 自最深向根逐级派发
    MouseEvent press;
    press.position = Point{.x = 20.0F, .y = 20.0F};  // box1 内
    press.action = MouseAction::Press;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, press, nullptr));
    AURORA_TEST_CHECK_EQ(tree.box1->press_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->pointer_events, 1);
    AURORA_TEST_CHECK_FALSE(press.is_handled);

    MouseEvent release;
    release.position = press.position;
    release.action = MouseAction::Release;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, release, nullptr));
    AURORA_TEST_CHECK_EQ(tree.box1->release_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->pointer_events, 2);

    // 命中最深控件消费后：冒泡终止，根不再收到
    tree.box1->consume_pointer = true;
    MouseEvent press2;
    press2.position = press.position;
    press2.action = MouseAction::Press;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, press2, nullptr));
    AURORA_TEST_CHECK_TRUE(press2.is_handled);
    AURORA_TEST_CHECK_EQ(tree.row->pointer_events, 2);  // 未增加
    AURORA_TEST_CHECK_EQ(tree.box2->press_count, 0);  // 兄弟控件不受影响

    MouseEvent release2;
    release2.position = press.position;
    release2.action = MouseAction::Release;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, release2, nullptr));
    AURORA_TEST_CHECK_TRUE(release2.is_handled);
    AURORA_TEST_CHECK_EQ(tree.row->pointer_events, 2);
}

AURORA_TEST_CASE(mouse_press_localizes_coordinates_and_updates_focus) {
    auto tree = make_tree();
    EventDispatcher dispatcher;
    FocusManager fm;

    // box2 全局原点 (40,0)：全局 (45,10) → 本地 (5,10)；根原点 (0,0) 本地即全局
    MouseEvent press;
    press.position = Point{.x = 45.0F, .y = 10.0F};
    press.action = MouseAction::Press;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, press, &fm));
    AURORA_TEST_CHECK_NEAR(tree.box2->last_local.x, 5.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.box2->last_local.y, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.row->last_local.x, 45.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(tree.row->last_local.y, 10.0F, 1e-4F);
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());  // 命中链最近可获焦者获焦

    // 点击空白：清除焦点且不命中
    MouseEvent blank;
    blank.position = Point{.x = 500.0F, .y = 500.0F};
    blank.action = MouseAction::Press;
    AURORA_TEST_CHECK_FALSE(dispatcher.dispatch_mouse(*tree.row, blank, &fm));
    AURORA_TEST_CHECK(fm.focused() == nullptr);
}

AURORA_TEST_CASE(pointer_capture_delivers_beyond_root_bounds) {
    auto tree = make_tree();
    EventDispatcher dispatcher;

    MouseEvent press;
    press.position = Point{.x = 20.0F, .y = 20.0F};
    press.action = MouseAction::Press;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, press));

    // 拖出根矩形：捕获链仍把事件路由给按下的目标
    MouseEvent move;
    move.position = Point{.x = 500.0F, .y = 500.0F};
    move.action = MouseAction::Move;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, move));
    AURORA_TEST_CHECK_EQ(tree.box1->move_count, 1);
    AURORA_TEST_CHECK_EQ(tree.box2->move_count, 0);

    MouseEvent release;
    release.position = move.position;
    release.action = MouseAction::Release;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, release));
    AURORA_TEST_CHECK_EQ(tree.box1->release_count, 1);

    // Release 已解除捕获：此后窗外 Move 不再派发
    MouseEvent after;
    after.position = move.position;
    after.action = MouseAction::Move;
    AURORA_TEST_CHECK_FALSE(dispatcher.dispatch_mouse(*tree.row, after));
    AURORA_TEST_CHECK_EQ(tree.box1->move_count, 1);
}

AURORA_TEST_CASE(hover_move_diffs_enter_and_leave) {
    auto tree = make_tree();
    EventDispatcher dispatcher;

    MouseEvent over_box1;
    over_box1.position = Point{.x = 20.0F, .y = 20.0F};
    over_box1.action = MouseAction::Move;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, over_box1));
    AURORA_TEST_CHECK_EQ(tree.box1->hover_changes, 1);  // 进入
    AURORA_TEST_CHECK_TRUE(tree.box1->hovered());
    AURORA_TEST_CHECK_TRUE(tree.row->hovered());

    // 同一命中链内的再次移动：不重复通知
    MouseEvent again;
    again.position = Point{.x = 30.0F, .y = 30.0F};
    again.action = MouseAction::Move;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, again));
    AURORA_TEST_CHECK_EQ(tree.box1->hover_changes, 1);

    // 移到空白：离开通知，悬停态清除
    MouseEvent blank;
    blank.position = Point{.x = 500.0F, .y = 500.0F};
    blank.action = MouseAction::Move;
    AURORA_TEST_CHECK_FALSE(dispatcher.dispatch_mouse(*tree.row, blank));
    AURORA_TEST_CHECK_EQ(tree.box1->hover_changes, 2);  // 离开
    AURORA_TEST_CHECK_FALSE(tree.box1->hovered());
    AURORA_TEST_CHECK_FALSE(tree.row->hovered());
}

AURORA_TEST_CASE(hover_cursor_resolves_and_emits_on_change) {
    auto tree = make_tree();
    EventDispatcher dispatcher;
    std::vector<CursorShape> emitted;
    dispatcher.set_cursor_handler([&emitted](CursorShape s) { emitted.push_back(s); });

    auto move_to = [&](float x, float y) {
        MouseEvent m;
        m.position = Point{.x = x, .y = y};
        m.action = MouseAction::Move;
        dispatcher.dispatch_mouse(*tree.row, m);
    };

    // 1) 无任何声明：首次解析也回调（Arrow），同形状重复移动不重复下发
    move_to(20.0F, 20.0F);  // box1 内
    AURORA_TEST_CHECK_EQ(emitted.size(), 1);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::Arrow);
    move_to(30.0F, 30.0F);  // 仍在 box1，链与声明均未变
    AURORA_TEST_CHECK_EQ(emitted.size(), 1);

    // 2) 修饰链 CursorNode 声明生效（悬停驱动的重解析：再次 Move 触发）
    tree.box1->modifier = Modifier{}.cursor(CursorShape::IBeam);
    move_to(25.0F, 25.0F);
    AURORA_TEST_CHECK_EQ(emitted.size(), 2);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::IBeam);

    // 3) Widget 虚钩子生效；修饰链声明优先于钩子
    tree.box2->cursor_hook = CursorShape::Crosshair;
    move_to(60.0F, 20.0F);  // box2 内
    AURORA_TEST_CHECK_EQ(emitted.size(), 3);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::Crosshair);
    tree.box2->modifier = Modifier{}.cursor(CursorShape::Wait);
    move_to(65.0F, 25.0F);
    AURORA_TEST_CHECK_EQ(emitted.size(), 4);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::Wait);

    // 4) 含 Clickable 修饰且无声明/钩子 → 缺省 PointingHand
    tree.box2->modifier = Modifier{};
    tree.box2->cursor_hook.reset();
    tree.box2->modifier = Modifier{}.clickable([] {});
    move_to(60.0F, 20.0F);
    AURORA_TEST_CHECK_EQ(emitted.size(), 5);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::PointingHand);

    // 5) 内层覆盖外层：根声明 Move，box2 无声明 → 沿命中链回溯取根的 Move；
    //    移到空白（链空）→ 回落 Arrow
    tree.box2->modifier = Modifier{};
    tree.row->modifier = Modifier{}.cursor(CursorShape::Move);
    move_to(60.0F, 20.0F);
    AURORA_TEST_CHECK_EQ(emitted.size(), 6);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::Move);
    move_to(500.0F, 500.0F);  // 空白
    AURORA_TEST_CHECK_EQ(emitted.size(), 7);
    AURORA_TEST_CHECK_EQ(emitted.back(), CursorShape::Arrow);
}

AURORA_TEST_CASE(key_dispatch_tab_navigation_and_activation_shortcuts) {
    auto tree = make_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());

    // Tab：无焦点 → 第一个候选（根不可聚焦，候选 = [box1, box2]）
    KeyEvent tab;
    tab.key = static_cast<int>(KeyCode::Tab);
    tab.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, tab, fm));
    AURORA_TEST_CHECK_TRUE(tab.is_handled);
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());

    KeyEvent tab2;
    tab2.key = static_cast<int>(KeyCode::Tab);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, tab2, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());

    // Shift+Tab 后退
    KeyEvent shift_tab;
    shift_tab.key = static_cast<int>(KeyCode::Tab);
    shift_tab.modifiers = ModifierKey::Shift;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, shift_tab, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());

    // Enter / Space 激活焦点控件
    KeyEvent enter;
    enter.key = static_cast<int>(KeyCode::Enter);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, enter, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 1);

    KeyEvent space;
    space.key = static_cast<int>(KeyCode::Space);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, space, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 2);

    // 普通按键路由到焦点控件
    KeyEvent letter;
    letter.key = static_cast<int>(KeyCode::A);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, letter, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 1);

    // 焦点控件不消费 → dispatch 返回 false
    tree.box1->consume_keys = false;
    KeyEvent letter2;
    letter2.key = static_cast<int>(KeyCode::A);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, letter2, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 2);

    // 无焦点：普通键与激活键均不消费
    fm.clear();
    KeyEvent no_focus;
    no_focus.key = static_cast<int>(KeyCode::A);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, no_focus, fm));
    KeyEvent no_focus_enter;
    no_focus_enter.key = static_cast<int>(KeyCode::Enter);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, no_focus_enter, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 2);
}

AURORA_TEST_CASE(scroll_text_input_and_file_drop_route_to_target) {
    auto tree = make_tree();
    FocusManager fm;

    // 滚轮：只给命中最深叶（不冒泡）；窗外未命中 → false
    ScrollEvent scroll;
    scroll.position = Point{.x = 20.0F, .y = 20.0F};
    scroll.delta_y = -3.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, scroll));
    AURORA_TEST_CHECK_EQ(tree.box1->scroll_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->scroll_count, 0);  // 不冒泡
    AURORA_TEST_CHECK_TRUE(scroll.is_handled);

    ScrollEvent blank_scroll;
    blank_scroll.position = Point{.x = 500.0F, .y = 500.0F};
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, blank_scroll));
    AURORA_TEST_CHECK_EQ(tree.box1->scroll_count, 1);

    // 文本输入：路由到焦点控件；无焦点 → false
    fm.set_focus(tree.box1.get());
    TextInputEvent text;
    text.text = "x";
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, text, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->text_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->text_count, 0);  // 只给焦点控件

    fm.clear();
    TextInputEvent no_focus;
    no_focus.text = "y";
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, no_focus, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->text_count, 1);

    // IME 组合事件：与 TextInputEvent 同构路由——只给焦点控件、不冒泡；无焦点 → false
    fm.set_focus(tree.box1.get());
    TextCompositionEvent composition;
    composition.preedit = "nihao";
    composition.cursor_index = 5;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, composition, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->composition_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->composition_count, 0);  // 不冒泡
    AURORA_TEST_CHECK_TRUE(composition.is_handled);

    fm.clear();
    TextCompositionEvent no_focus_composition;
    no_focus_composition.preedit = "ni";
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, no_focus_composition, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->composition_count, 1);

    // 文件拖放：命中即调用 on_file_drop；返回值 = 是否被消费
    FileDropEvent drop;
    drop.position = Point{.x = 20.0F, .y = 20.0F};
    drop.paths = {"a.txt"};
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, drop));  // 未消费
    AURORA_TEST_CHECK_EQ(tree.box1->drop_count, 1);

    tree.box1->consume_drop = true;
    FileDropEvent drop2;
    drop2.position = Point{.x = 20.0F, .y = 20.0F};
    drop2.paths = {"b.txt"};
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, drop2));
    AURORA_TEST_CHECK_EQ(tree.box1->drop_count, 2);

    FileDropEvent blank_drop;
    blank_drop.position = Point{.x = 500.0F, .y = 500.0F};
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(*tree.row, blank_drop));
    AURORA_TEST_CHECK_EQ(tree.box1->drop_count, 2);
}

AURORA_TEST_CASE(touch_dispatcher_captures_and_synthesizes_per_pointer) {
    auto tree = make_tree();
    TouchDispatcher dispatcher;

    // 完全未命中：返回 false
    TouchEvent miss;
    miss.points.push_back(TouchPoint{.id = 9, .position = Point{.x = 500.0F, .y = 500.0F}});
    AURORA_TEST_CHECK_FALSE(dispatcher.dispatch(*tree.row, miss));
    AURORA_TEST_CHECK_EQ(tree.box1->press_count, 0);

    // 触点按下：合成 MouseEvent::Press（携带 pointer_id）发给命中目标
    TouchEvent down;
    down.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 20.0F, .y = 20.0F}});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch(*tree.row, down));
    AURORA_TEST_CHECK_EQ(tree.box1->press_count, 1);
    AURORA_TEST_REQUIRE(tree.box1->last_pointer_id.has_value());
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(tree.box1->last_pointer_id.value(), 1);

    // 按住移出根矩形：按指针 id 的捕获链保持，继续派发 Move
    TouchEvent drag;
    drag.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 500.0F, .y = 500.0F}});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch(*tree.row, drag));
    AURORA_TEST_CHECK_EQ(tree.box1->move_count, 1);
    AURORA_TEST_CHECK_EQ(tree.box2->press_count, 0);

    // 第二指落在另一控件：各指针独立捕获（id1 复用链 → Move；id2 新按下 → Press）
    TouchEvent second;
    second.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 500.0F, .y = 500.0F}});
    second.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 60.0F, .y = 20.0F}});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch(*tree.row, second));
    AURORA_TEST_CHECK_EQ(tree.box1->move_count, 2);
    AURORA_TEST_CHECK_EQ(tree.box2->press_count, 1);

    // 双指抬起：各自收到 Release，捕获清除
    TouchEvent up;
    up.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 500.0F, .y = 500.0F}, .is_active = false});
    up.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 60.0F, .y = 20.0F}, .is_active = false});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch(*tree.row, up));
    AURORA_TEST_CHECK_EQ(tree.box1->release_count, 1);
    AURORA_TEST_CHECK_EQ(tree.box2->release_count, 1);

    // 抬起后再按下：重新命中，新一轮 Press
    TouchEvent redown;
    redown.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 20.0F, .y = 20.0F}});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch(*tree.row, redown));
    AURORA_TEST_CHECK_EQ(tree.box1->press_count, 2);
}

}  // namespace aurora::test_cases::utest_dispatcher
