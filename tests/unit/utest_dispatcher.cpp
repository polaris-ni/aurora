/// 测试类型: unit
/// 目标单元: include/aurora/event/dispatcher.h
/// 测试说明: 命中测试最深目标、鼠标冒泡与 stop-on-handled、本地坐标写入与 Press
/// 焦点转移/空白清焦（并核对 Press 与 Tab 各自记录的焦点到达方式，它决定基类焦点环显隐）、
/// 指针捕获越界续发、悬停进出 diff、悬停光标解析
/// （修饰链 > 虚钩子 > Clickable 缺省，变化才下发）、键盘
/// Tab/激活快捷键与焦点路由（含激活键 / 方向键优先投递 on_key_event 的控件级 opt-in，
/// 以及真实 TextInput 的方向键归光标、不夺焦点）、滚轮/文本/文件拖放路由、
/// 滚轮余量自最深可滚动者上冒给更浅祖先（嵌套滚动协调：内层到顶后外层下拉刷新接手）、
/// TouchDispatcher 按指针 id 捕获与合成鼠标事件、连击序号（click_count）在 Press 上累加并在
/// Release / Move 上恒为 1、超窗或位移过大重置、上限封顶为 3、左右键与多指针各自独立计数

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/event/dispatcher.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/pull_to_refresh.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text_input.h"
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
    bool activation_keys_to_key_event = false;  ///< 覆写 wants_activation_keys() 的开关
    bool navigation_keys_to_key_event = false;  ///< 覆写 wants_navigation_keys() 的开关
    bool tab_keys_to_key_event = false;  ///< 覆写 wants_tab_keys() 的开关

    int press_count = 0;
    int release_count = 0;
    int move_count = 0;
    int key_count = 0;
    std::uint8_t last_click_count = 0;  ///< 最近一次指针事件携带的连击序号（供 click_count 用例断言）
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

    auto type_name() const -> const char * override { return "TestBox"; }

    auto on_layout(const Constraints &c, [[maybe_unused]] const BuildContext &ctx) -> Size override {
        size_ = c.constrain(Size{.width = box_width, .height = box_height});
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter &p, [[maybe_unused]] const Rect &bounds,
                  [[maybe_unused]] const BuildContext &ctx) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
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
        last_click_count = e.click_count;
        if (consume_pointer) {
            e.is_handled = true;
        }
    }

    auto on_key_event(KeyEvent &e) -> void override {
        ++key_count;
        if (consume_keys) {
            e.is_handled = true;
        }
    }

    auto activate() -> void override { ++activations; }

    [[nodiscard]] auto wants_activation_keys() const -> bool override { return activation_keys_to_key_event; }

    [[nodiscard]] auto wants_navigation_keys() const -> bool override { return navigation_keys_to_key_event; }

    [[nodiscard]] auto wants_tab_keys() const -> bool override { return tab_keys_to_key_event; }

    auto on_hover_change(bool entered) -> void override {
        ++hover_changes;
        Widget::on_hover_change(entered);
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        ++scroll_count;
        Widget::on_scroll(e);  // 默认消费
    }

    auto on_text_input(TextInputEvent &e) -> void override {
        ++text_count;
        Widget::on_text_input(e);  // 默认消费
    }

    auto on_text_composition(TextCompositionEvent &e) -> void override {
        ++composition_count;
        Widget::on_text_composition(e);  // 默认消费
    }

    auto on_file_drop(FileDropEvent &e) -> void override {
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

    auto type_name() const -> const char * override { return "TestRow"; }

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        float x = 0.0F;
        float max_height = 0.0F;
        for (Node &ch : children_) {
            const Size cs = ch.widget().layout(Constraints{.min = Size{}, .max = c.max}, ctx);
            ch.set_bounds(Rect{.origin = Point{.x = x, .y = 0.0F}, .size = cs});
            x += cs.width;
            max_height = std::max(max_height, cs.height);
        }
        size_ = c.constrain(Size{.width = x, .height = max_height});
        return size_;
    }

    auto on_paint([[maybe_unused]] Painter &p, [[maybe_unused]] const Rect &bounds,
                  [[maybe_unused]] const BuildContext &ctx) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
        ++pointer_events;
        last_local = e.local_position;
        if (consume_pointer) {
            e.is_handled = true;
        }
    }

    auto on_scroll(ScrollEvent &e) -> void override {
        ++scroll_count;
        Widget::on_scroll(e);
    }

    auto on_text_input(TextInputEvent &e) -> void override {
        ++text_count;
        Widget::on_text_input(e);
    }

    auto on_text_composition(TextCompositionEvent &e) -> void override {
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

struct NestedScrollTree {
    std::shared_ptr<PullToRefresh> outer;
    std::shared_ptr<Scroll> inner;
};

/// 外层下拉刷新包住内层滚动：视口 300×300、内容 800 → 内层可滚 [0, 500]（step=1 便于按 dp 推算）。
auto make_nested_scroll_tree() -> NestedScrollTree {
    auto content = std::make_shared<TestBox>();
    content->box_width = 300.0F;
    content->box_height = 800.0F;
    auto inner = std::make_shared<Scroll>(ScrollProps{.child = Node{content}, .step = 1.0F});
    auto outer = std::make_shared<PullToRefresh>(Node{inner});
    LayoutEngine::layout(*outer, Constraints{.min = Size{}, .max = Size{.width = 300.0F, .height = 300.0F}});
    return NestedScrollTree{.outer = outer, .inner = inner};
}

/// 真实文本框与一个兄弟并排：兄弟画在输入框的左上方 (0,-40,40,40)，输入框占 (40,0,40,40)。
/// 于是 ← 的几何目标存在（若方向键被焦点导航吃掉，焦点就会移过去），而 ↑ 才是该用的导航键。
/// 焦点导航取的是「最近一次绘制的绝对盒」，本夹具不走绘制，故经 seam 手工给出。
struct FieldTree {
    std::shared_ptr<TestRow> row;
    std::shared_ptr<TestBox> left;
    std::shared_ptr<TextInput> field;
};

auto make_field_tree() -> FieldTree {
    auto row = std::make_shared<TestRow>();
    row->set_focusable(false);
    auto left = std::make_shared<TestBox>();
    auto field = std::make_shared<TextInput>();
    row->add(Node{left});
    row->add(Node{field});
    left->set_focus_bounds(
        Rect{.origin = Point{.x = 0.0F, .y = -40.0F}, .size = Size{.width = 40.0F, .height = 40.0F}});
    field->set_focus_bounds(
        Rect{.origin = Point{.x = 40.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 40.0F}});
    return FieldTree{.row = row, .left = left, .field = field};
}

}  // namespace

AURORA_TEST_CASE(hit_test_finds_deepest_and_misses_blank) {
    const auto tree = make_tree();

    const auto *left = EventDispatcher::hit_test(*tree.row, Point{.x = 20.0F, .y = 20.0F});
    AURORA_TEST_CHECK(left == tree.box1.get());

    const auto *right = EventDispatcher::hit_test(*tree.row, Point{.x = 60.0F, .y = 20.0F});
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

AURORA_TEST_CASE(pointer_press_and_tab_record_their_focus_arrival_modality) {
    // 焦点环显隐的接线口在派发层：指针按下记 Pointer（不出环，控件已有 pressed 反馈），键盘 Tab
    // 记 Keyboard（必出环，无障碍停点的唯一可见线索）。见 Widget::focus_ring_shown 与规格 §4.4。
    auto tree = make_tree();
    EventDispatcher dispatcher;
    FocusManager fm;
    fm.set_root(tree.row.get());

    MouseEvent press;
    press.position = Point{.x = 45.0F, .y = 10.0F};  // box2 内
    press.action = MouseAction::Press;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, press, &fm));
    AURORA_TEST_REQUIRE(fm.focused() == tree.box2.get());
    AURORA_TEST_CHECK(tree.box2->focus_arrival() == FocusArrival::Pointer);
    AURORA_TEST_CHECK_FALSE(tree.box2->focus_ring_shown());

    // 点击之后立刻按 Tab：键盘模态必须当场把可见停点带回来（候选 [box1, box2]，自 box2 回卷到 box1）。
    KeyEvent tab;
    tab.key = static_cast<int>(KeyCode::Tab);
    tab.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, tab, fm));
    AURORA_TEST_REQUIRE(fm.focused() == tree.box1.get());
    AURORA_TEST_CHECK(tree.box1->focus_arrival() == FocusArrival::Keyboard);
    AURORA_TEST_CHECK_MSG(tree.box1->focus_ring_shown(),
                          "keyboard navigation after a click must restore the visible focus stop");
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

AURORA_TEST_CASE(arrow_keys_honour_the_navigation_keys_opt_in) {
    auto tree = make_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());
    // 方向键导航按「最近一次绘制的绝对盒」取几何（本夹具不走绘制），经测试 seam 手工给出。
    tree.box1->set_focus_bounds(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 40.0F}});
    tree.box2->set_focus_bounds(
        Rect{.origin = Point{.x = 40.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 40.0F}});
    fm.set_focus(tree.box1.get());

    auto press_right = []() -> KeyEvent {
        KeyEvent e;
        e.key = static_cast<int>(KeyCode::ArrowRight);
        e.action = KeyAction::Down;
        return e;
    };

    // 默认（未 opt-in）：方向键 = 几何焦点导航，焦点控件观察不到按键。
    KeyEvent baseline = press_right();
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, baseline, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 0);

    // opt-in 且消费：按键先到控件，焦点不动（列表内部光标 / 键盘重排依赖此路由）。
    fm.set_focus(tree.box1.get());
    tree.box1->navigation_keys_to_key_event = true;
    tree.box1->consume_keys = true;
    KeyEvent claimed = press_right();
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, claimed, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 1);
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());

    // opt-in 但不消费（控件不认领该方向）：回落几何焦点导航。
    tree.box1->consume_keys = false;
    KeyEvent unclaimed = press_right();
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, unclaimed, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 2);
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());
}

// 方向键在真实文本框上的路由：←/→ 归光标，未被认领的 ↑/↓ 才回落几何焦点导航。
// 这一条守住的正是「opt-in 谓词默认 false 时，文本框方向键被焦点导航吃掉」的缺陷：
// 焦点会移到左侧兄弟，此后所有按键（含退格）都落到别的控件上，输入框失能。
AURORA_TEST_CASE(direction_keys_reach_a_focused_text_input_before_focus_navigation) {
    auto tree = make_field_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());
    const BuildContext ctx;
    tree.field->mount(ctx);
    tree.field->layout(Constraints{.min = Size{}, .max = Size{.width = 40.0F, .height = 40.0F}}, ctx);
    fm.set_focus(tree.field.get());

    auto key = [](int code, ModifierKey mods) -> KeyEvent {
        KeyEvent e;
        e.key = code;
        e.action = KeyAction::Down;
        e.modifiers = mods;
        return e;
    };
    auto type = [](const std::string &t) -> TextInputEvent {
        TextInputEvent e;
        e.text = t;
        return e;
    };

    // 种子输入：caret 在末尾。
    TextInputEvent seed = type("abc");
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, seed, fm));
    AURORA_TEST_CHECK_EQ(tree.field->value(), std::string{"abc"});

    // ←：焦点必须留在输入框，兄弟控件观察不到按键（缺陷形态是焦点被移走）。
    KeyEvent left = key(static_cast<int>(KeyCode::ArrowLeft), ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, left, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.field.get());
    AURORA_TEST_CHECK_EQ(tree.left->key_count, 0);

    // 光标真的左移了一位：新字符插在光标处，而非追加末尾。
    TextInputEvent ins = type("X");
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, ins, fm));
    AURORA_TEST_CHECK_EQ(tree.field->value(), std::string{"abXc"});

    // Shift+→：选区落在光标右侧那个字符上，焦点仍不动。
    KeyEvent shift_right = key(static_cast<int>(KeyCode::ArrowRight), ModifierKey::Shift);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, shift_right, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.field.get());
    AURORA_TEST_CHECK_TRUE(tree.field->has_selection());
    AURORA_TEST_CHECK_EQ(tree.field->selected_text(), std::string{"c"});

    // 退格：一次删除整个选区（而非仅左删一个字符）。
    KeyEvent backspace = key(static_cast<int>(KeyCode::Backspace), ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, backspace, fm));
    AURORA_TEST_CHECK_EQ(tree.field->value(), std::string{"abX"});
    AURORA_TEST_CHECK_FALSE(tree.field->has_selection());

    // ↑ 不在文本框的按键表里：照旧回落几何焦点导航，焦点移到上方候选。
    // （兄弟摆在左上方，正是为了让 ↑ 既有几何目标、又能检验 ← 未被导航吃掉。）
    KeyEvent up = key(static_cast<int>(KeyCode::ArrowUp), ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, up, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.left.get());
}

AURORA_TEST_CASE(tab_keys_honour_the_tab_keys_opt_in) {
    auto tree = make_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());

    auto press_tab = [](ModifierKey mods) -> KeyEvent {
        KeyEvent e;
        e.key = static_cast<int>(KeyCode::Tab);
        e.action = KeyAction::Down;
        e.modifiers = mods;
        return e;
    };

    // (c) 未覆写 wants_tab_keys()：Tab 序遍历，焦点控件观察不到按键（既有语义逐位不变）。
    KeyEvent baseline = press_tab(ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, baseline, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 0);
    KeyEvent baseline_next = press_tab(ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, baseline_next, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 0);

    // (a) 覆写为 true 且 on_key_event 消费：按键先到控件，焦点不动，派发返回已处理。
    fm.set_focus(tree.box1.get());
    tree.box1->tab_keys_to_key_event = true;
    tree.box1->consume_keys = true;
    KeyEvent claimed = press_tab(ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, claimed, fm));
    AURORA_TEST_CHECK_TRUE(claimed.is_handled);
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 1);
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());

    // (b) 覆写为 true 但不消费：回落焦点序遍历，焦点按方向前移。
    tree.box1->consume_keys = false;
    KeyEvent unclaimed = press_tab(ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, unclaimed, fm));
    AURORA_TEST_CHECK_TRUE(unclaimed.is_handled);
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 2);
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());

    // (d) Shift+Tab 后退方向在 (a) 消费态下正确：焦点控件消费即止，不做后退。
    fm.set_focus(tree.box2.get());
    tree.box2->tab_keys_to_key_event = true;
    tree.box2->consume_keys = true;
    KeyEvent shift_claimed = press_tab(ModifierKey::Shift);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, shift_claimed, fm));
    AURORA_TEST_CHECK_EQ(tree.box2->key_count, 1);
    AURORA_TEST_CHECK(fm.focused() == tree.box2.get());

    // (d) Shift+Tab 后退方向在 (b) 不消费态下正确：回落后退到 box1。
    tree.box2->consume_keys = false;
    KeyEvent shift_unclaimed = press_tab(ModifierKey::Shift);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, shift_unclaimed, fm));
    AURORA_TEST_CHECK_EQ(tree.box2->key_count, 2);
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());
}

// Tab 序遍历的「仅按下阶段」口径：KeyAction::Up 不匹配全局快捷键，直接落焦点控件。
// 这一条守住 wants_tab_keys() 前置投递没有被误放到释放阶段（否则控件会收到两次 Tab）。
AURORA_TEST_CASE(tab_key_opt_in_only_applies_to_key_down) {
    auto tree = make_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());
    tree.box1->tab_keys_to_key_event = true;
    tree.box1->consume_keys = true;
    fm.set_focus(tree.box1.get());

    KeyEvent up;
    up.key = static_cast<int>(KeyCode::Tab);
    up.action = KeyAction::Up;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, up, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 1);
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());
}

AURORA_TEST_CASE(activation_keys_route_to_key_event_for_opt_in_widgets) {
    auto tree = make_tree();
    FocusManager fm;
    fm.set_root(tree.row.get());

    // 聚焦 box1（根不可聚焦，首个候选即 box1）。
    KeyEvent tab;
    tab.key = static_cast<int>(KeyCode::Tab);
    tab.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, tab, fm));
    AURORA_TEST_CHECK(fm.focused() == tree.box1.get());

    // 默认（未 opt-in）：Enter 直接激活，不进入 on_key_event（既有语义不变）。
    KeyEvent enter;
    enter.key = static_cast<int>(KeyCode::Enter);
    enter.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, enter, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 1);
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 0);

    // opt-in 且 on_key_event 消费：Enter 只走键盘入口，不再触发激活。
    tree.box1->activation_keys_to_key_event = true;
    KeyEvent enter_opt_in;
    enter_opt_in.key = static_cast<int>(KeyCode::Enter);
    enter_opt_in.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, enter_opt_in, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 1);
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 1);

    // opt-in 但 on_key_event 不消费：回落激活语义（Space 同理）。
    tree.box1->consume_keys = false;
    KeyEvent space;
    space.key = static_cast<int>(KeyCode::Space);
    space.action = KeyAction::Down;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, space, fm));
    AURORA_TEST_CHECK_EQ(tree.box1->key_count, 2);
    AURORA_TEST_CHECK_EQ(tree.box1->activations, 2);
}

AURORA_TEST_CASE(scroll_text_input_and_file_drop_route_to_target) {
    auto tree = make_tree();
    FocusManager fm;

    // 滚轮：链上无可滚动者时兜底交给点命中目标，且不冒泡（窗外未命中 → false）。
    ScrollEvent scroll;
    scroll.position = Point{.x = 20.0F, .y = 20.0F};
    scroll.delta_y = -3.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*tree.row, scroll));
    AURORA_TEST_CHECK_EQ(tree.box1->scroll_count, 1);
    AURORA_TEST_CHECK_EQ(tree.row->scroll_count, 0);  // 未回传余量：就此止步
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

AURORA_TEST_CASE(wheel_margin_bubbles_from_inner_scroll_to_pull_to_refresh) {
    constexpr Point center{.x = 150.0F, .y = 150.0F};

    // 内层已在顶部：向上滚的全量作为余量上冒，外层按橡皮筋折算为下拉距离。
    auto at_top = make_nested_scroll_tree();
    ScrollEvent bubble;
    bubble.position = center;
    bubble.delta_y = 5.0F;  // 5 单位 × 16dp（库内滚轮步长口径）= 80dp 物理下拉
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*at_top.outer, bubble));
    AURORA_TEST_CHECK_NEAR(at_top.inner->offset_y(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(at_top.outer->pull_distance(), 49.23F, 0.01F);  // 80·128/(80+128)
    AURORA_TEST_CHECK_EQ(static_cast<int>(at_top.outer->state()), static_cast<int>(PullToRefreshState::Pulling));

    // 内层离顶且能吃尽请求：余量为 0，外层不动（下拉手势不与正常滚动抢手）。
    auto mid = make_nested_scroll_tree();
    mid.inner->set_offset(120.0F);
    ScrollEvent inner_only;
    inner_only.position = center;
    inner_only.delta_y = 1.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*mid.outer, inner_only));
    AURORA_TEST_CHECK_NEAR(mid.inner->offset_y(), 119.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(mid.outer->pull_distance(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(mid.outer->state()), static_cast<int>(PullToRefreshState::Idle));

    // 内层只吃掉 120dp（200 单位请求中的 120 单位）：余 80 单位上冒，外层折算 1280dp 橡皮筋输入。
    auto partial = make_nested_scroll_tree();
    partial.inner->set_offset(120.0F);
    ScrollEvent over_top;
    over_top.position = center;
    over_top.delta_y = 200.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*partial.outer, over_top));
    AURORA_TEST_CHECK_NEAR(partial.inner->offset_y(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(partial.outer->pull_distance(), 116.36F, 0.01F);  // 1280·128/(1280+128)

    // 向下滚（露出下方内容）永不算下拉：内层自身消费，外层保持空闲。
    auto down = make_nested_scroll_tree();
    ScrollEvent downward;
    downward.position = center;
    downward.delta_y = -5.0F;
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(*down.outer, downward));
    AURORA_TEST_CHECK_NEAR(down.inner->offset_y(), 5.0F, 1e-4F);  // step=1：5 单位 = 5dp
    AURORA_TEST_CHECK_NEAR(down.outer->pull_distance(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(click_count_accumulates_on_press_and_resets_on_release) {
    // 连击序号只在 Press 上累加，且以「上次 Release」为比对基准：按住不放不产生连击。
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;  // 窗口恒开：隔离时间因素，只验计数与重置语义

    MouseEvent first;
    first.action = MouseAction::Press;
    first.position = Point{.x = 10.0F, .y = 10.0F};
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, first));
    AURORA_TEST_CHECK_EQ(static_cast<int>(first.click_count), 1);

    MouseEvent up = first;
    up.action = MouseAction::Release;
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, up));
    AURORA_TEST_CHECK_EQ(static_cast<int>(up.click_count), 1);  // Release 恒为 1

    MouseEvent second;
    second.action = MouseAction::Press;
    second.position = Point{.x = 10.0F, .y = 10.0F};
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, second));
    AURORA_TEST_CHECK_EQ(static_cast<int>(second.click_count), 2);

    MouseEvent up2 = second;
    up2.action = MouseAction::Release;
    dispatcher.dispatch_mouse(*tree.row, up2);

    MouseEvent third;
    third.action = MouseAction::Press;
    third.position = Point{.x = 10.0F, .y = 10.0F};
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(*tree.row, third));
    AURORA_TEST_CHECK_EQ(static_cast<int>(third.click_count), 3);
}

AURORA_TEST_CASE(click_count_caps_at_three) {
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;
    const Point at{.x = 10.0F, .y = 10.0F};

    auto last = 0;
    for (int i = 1; i <= 5; ++i) {
        MouseEvent down;
        down.action = MouseAction::Press;
        down.position = at;
        dispatcher.dispatch_mouse(*tree.row, down);
        last = static_cast<int>(down.click_count);
        MouseEvent up = down;
        up.action = MouseAction::Release;
        dispatcher.dispatch_mouse(*tree.row, up);
    }
    // 更快的连续点击仍记 AURORA_MAX_CLICK_COUNT，供「三击选整段」语义使用。
    AURORA_TEST_CHECK_EQ(last, static_cast<int>(AURORA_MAX_CLICK_COUNT));
}

AURORA_TEST_CASE(click_count_resets_when_position_moves_beyond_radius) {
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;
    dispatcher.click_radius_dp = 4.0F;

    MouseEvent a;
    a.action = MouseAction::Press;
    a.position = Point{.x = 10.0F, .y = 10.0F};
    dispatcher.dispatch_mouse(*tree.row, a);
    MouseEvent up = a;
    up.action = MouseAction::Release;
    dispatcher.dispatch_mouse(*tree.row, up);

    // 位移 5dp > 半径 4dp：判为新一次点击序列。判据取窗口逻辑坐标 position。
    MouseEvent b;
    b.action = MouseAction::Press;
    b.position = Point{.x = 15.0F, .y = 10.0F};
    dispatcher.dispatch_mouse(*tree.row, b);
    AURORA_TEST_CHECK_EQ(static_cast<int>(b.click_count), 1);
}

AURORA_TEST_CASE(click_count_resets_when_window_expires) {
    // 窗口置 0：每次点击都重置为 1（极端阈值注入，避免测试依赖真实时钟推进）。
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 0;
    dispatcher.click_radius_dp = 100.0F;

    for (int i = 0; i < 3; ++i) {
        MouseEvent down;
        down.action = MouseAction::Press;
        down.position = Point{.x = 10.0F, .y = 10.0F};
        dispatcher.dispatch_mouse(*tree.row, down);
        AURORA_TEST_CHECK_EQ(static_cast<int>(down.click_count), 1);
        MouseEvent up = down;
        up.action = MouseAction::Release;
        dispatcher.dispatch_mouse(*tree.row, up);
    }
}

AURORA_TEST_CASE(click_count_is_tracked_per_button_and_pointer) {
    // 左/右键与不同指针各自独立计数：右键点击不得吃掉左键的连击序列。
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;
    const Point at{.x = 10.0F, .y = 10.0F};

    auto cycle = [&](MouseButton button, std::optional<int> pointer_id) -> int {
        MouseEvent down;
        down.action = MouseAction::Press;
        down.position = at;
        down.button = button;
        down.pointer_id = pointer_id;
        dispatcher.dispatch_mouse(*tree.row, down);
        MouseEvent up = down;
        up.action = MouseAction::Release;
        dispatcher.dispatch_mouse(*tree.row, up);
        return static_cast<int>(down.click_count);
    };

    AURORA_TEST_CHECK_EQ(cycle(MouseButton::Left, std::nullopt), 1);
    AURORA_TEST_CHECK_EQ(cycle(MouseButton::Right, std::nullopt), 1);  // 换键 → 独立序列
    AURORA_TEST_CHECK_EQ(cycle(MouseButton::Left, std::nullopt), 2);  // 回到左键 → 承接
    AURORA_TEST_CHECK_EQ(cycle(MouseButton::Left, 7), 1);  // 换指针 → 独立序列
}

AURORA_TEST_CASE(click_count_stays_one_for_move_events) {
    auto tree = make_tree();
    EventDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;

    MouseEvent down;
    down.action = MouseAction::Press;
    down.position = Point{.x = 10.0F, .y = 10.0F};
    dispatcher.dispatch_mouse(*tree.row, down);
    MouseEvent up = down;
    up.action = MouseAction::Release;
    dispatcher.dispatch_mouse(*tree.row, up);

    MouseEvent move;
    move.action = MouseAction::Move;
    move.position = Point{.x = 10.0F, .y = 10.0F};
    dispatcher.dispatch_mouse(*tree.row, move);
    AURORA_TEST_CHECK_EQ(static_cast<int>(move.click_count), 1);  // Move 不参与计数
}

AURORA_TEST_CASE(touch_double_tap_sets_click_count_on_synthesized_events) {
    // 触摸合成流同样过连击判定：双 tap 达 click_count=2，与鼠标双击同口径。
    auto tree = make_tree();
    TouchDispatcher dispatcher;
    dispatcher.click_window_ms = 60000;
    dispatcher.click_radius_dp = 100.0F;

    auto tap = [&](int id, bool active) -> void {
        TouchEvent te;
        TouchPoint p;
        p.id = id;
        p.position = Point{.x = 10.0F, .y = 10.0F};
        p.prev_position = p.position;
        p.is_active = active;
        te.points.push_back(p);
        dispatcher.dispatch(*tree.row, te);
    };

    tap(1, true);
    AURORA_TEST_CHECK_EQ(static_cast<int>(tree.box1->last_click_count), 1);
    tap(1, false);  // 抬起：标记本次点击完成
    AURORA_TEST_CHECK_EQ(static_cast<int>(tree.box1->last_click_count), 1);  // Release 恒为 1
    tap(1, true);
    AURORA_TEST_CHECK_EQ(static_cast<int>(tree.box1->last_click_count), 2);  // 双 tap
}

}  // namespace aurora::test_cases::utest_dispatcher
