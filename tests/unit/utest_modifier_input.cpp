/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_input.h
/// 测试说明: 覆盖 Input 切片六节点——Clickable 触发、Draggable 指针绑定/匹配/解绑与回调、
/// LongPress 阈值计时与幂等触发、TouchListener 原始流、TooltipNode 延迟显示、
/// ContextMenuNode 开合与位置记录

#include <chrono>

#include "aurora/modifier/modifier_input.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier_input {

namespace {

auto t_ms(int ms) -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
}

}  // namespace

AURORA_TEST_CASE(clickable_fires_callback) {
    int hits = 0;
    const Clickable c([&hits]() -> void { ++hits; });
    AURORA_TEST_CHECK_EQ(c.kind(), ModifierNode::Kind::Input);
    AURORA_TEST_CHECK_EQ(hits, 0);
    c.on_tap();
    c.fire_click();
    AURORA_TEST_CHECK_EQ(hits, 2);
}

AURORA_TEST_CASE(clickable_empty_callback_safe) {
    const Clickable c({});
    AURORA_TEST_CHECK_NO_THROW(c.fire_click());
}

AURORA_TEST_CASE(draggable_binds_pointer_and_filters_mismatched) {
    const Draggable d([](Point, Point) -> void {}, []() -> void {}, []() -> void {});
    // 未绑定视为任意指针。
    AURORA_TEST_CHECK_TRUE(d.matches(std::optional<int>(3)));
    d.bind(std::optional<int>(1));
    AURORA_TEST_CHECK_TRUE(d.matches(std::optional<int>(1)));
    AURORA_TEST_CHECK_FALSE(d.matches(std::optional<int>(2)));
    // 重复绑定不覆盖（首个指针持有）。
    d.bind(std::optional<int>(9));
    AURORA_TEST_CHECK_TRUE(d.matches(std::optional<int>(1)));
    // 抬起解绑后可重新绑定。
    d.release();
    AURORA_TEST_CHECK_TRUE(d.matches(std::optional<int>(2)));
    d.bind(std::optional<int>(2));
    AURORA_TEST_CHECK_TRUE(d.matches(std::optional<int>(2)));
}

AURORA_TEST_CASE(draggable_fires_start_drag_end_sequence) {
    int starts = 0;
    int drags = 0;
    int ends = 0;
    Point last_delta;
    Point last_pos;
    Draggable d(
        [&](Point delta, Point pos) -> void {
            ++drags;
            last_delta = delta;
            last_pos = pos;
        },
        [&]() -> void { ++starts; }, [&]() -> void { ++ends; });
    d.fire_start();
    d.fire_drag(Point{.x = 5.0F, .y = 3.0F}, Point{.x = 15.0F, .y = 13.0F});
    d.fire_end();
    AURORA_TEST_CHECK_EQ(starts, 1);
    AURORA_TEST_CHECK_EQ(drags, 1);
    AURORA_TEST_CHECK_EQ(ends, 1);
    AURORA_TEST_CHECK_NEAR(last_delta.x, 5.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(last_pos.x, 15.0F, 0.0F);
}

AURORA_TEST_CASE(long_press_fires_after_threshold_once) {
    int fires = 0;
    LongPress lp([&fires]() -> void { ++fires; }, 500.0F);
    AURORA_TEST_CHECK_FALSE(lp.long_press_fired());
    // 未按下时 tick 不触发。
    lp.tick(t_ms(10'000));
    AURORA_TEST_CHECK_EQ(fires, 0);
    // 按下后不足阈值不触发。
    lp.press_at(t_ms(1'000));
    lp.tick(t_ms(1'499));
    AURORA_TEST_CHECK_EQ(fires, 0);
    // 到达阈值触发一次。
    lp.tick(t_ms(1'500));
    AURORA_TEST_CHECK_EQ(fires, 1);
    AURORA_TEST_CHECK_TRUE(lp.long_press_fired());
    // 已触发后 tick 幂等。
    lp.tick(t_ms(2'500));
    AURORA_TEST_CHECK_EQ(fires, 1);
}

AURORA_TEST_CASE(long_press_cancel_prevents_firing) {
    int fires = 0;
    LongPress lp([&fires]() -> void { ++fires; }, 100.0F);
    lp.press_at(t_ms(1'000));
    lp.cancel();
    lp.tick(t_ms(9'999));
    AURORA_TEST_CHECK_EQ(fires, 0);
    // 取消后重新按下可再次进入计时。
    lp.press_at(t_ms(2'000));
    lp.tick(t_ms(2'101));
    AURORA_TEST_CHECK_EQ(fires, 1);
}

AURORA_TEST_CASE(long_press_pointer_binding) {
    LongPress lp([]() -> void {}, 100.0F);
    lp.bind(std::optional<int>(1));
    AURORA_TEST_CHECK_TRUE(lp.matches(std::optional<int>(1)));
    AURORA_TEST_CHECK_FALSE(lp.matches(std::optional<int>(2)));
    lp.release();
    AURORA_TEST_CHECK_TRUE(lp.matches(std::optional<int>(2)));
}

AURORA_TEST_CASE(touch_listener_delivers_raw_event) {
    int events = 0;
    int active = -1;
    const TouchListener tl([&](const TouchEvent& e) -> void {
        ++events;
        active = e.active_count();
    });
    AURORA_TEST_CHECK_EQ(tl.kind(), ModifierNode::Kind::Input);

    TouchEvent e;
    e.points.push_back(TouchPoint{.id = 0, .position = {}, .prev_position = {}, .is_active = true});
    e.points.push_back(TouchPoint{.id = 1, .position = {}, .prev_position = {}, .is_active = true});
    e.points.push_back(TouchPoint{.id = 2, .position = {}, .prev_position = {}, .is_active = false});
    tl.on_touch(e);
    AURORA_TEST_CHECK_EQ(events, 1);
    AURORA_TEST_CHECK_EQ(active, 2);
}

AURORA_TEST_CASE(tooltip_shows_after_delay_and_hides_on_leave) {
    const TooltipNode tt("hello", 300.0F);
    AURORA_TEST_CHECK_EQ(tt.kind(), ModifierNode::Kind::Input);
    AURORA_TEST_CHECK_EQ(tt.text(), "hello");
    AURORA_TEST_CHECK_NEAR(tt.delay_ms(), 300.0F, 0.0F);
    // 未悬停不可见。
    tt.tick(t_ms(100'000));
    AURORA_TEST_CHECK_FALSE(tt.is_visible());
    // 悬停不足延迟不可见。
    tt.hover_start(t_ms(1'000));
    tt.tick(t_ms(1'299));
    AURORA_TEST_CHECK_FALSE(tt.is_visible());
    // 到延迟后可见。
    tt.tick(t_ms(1'300));
    AURORA_TEST_CHECK_TRUE(tt.is_visible());
    // 离开重置。
    tt.hover_end();
    AURORA_TEST_CHECK_FALSE(tt.is_visible());
    // 重新悬停：可见性复位，需重新计满延迟。
    tt.hover_start(t_ms(5'000));
    tt.tick(t_ms(5'100));
    AURORA_TEST_CHECK_FALSE(tt.is_visible());
}

AURORA_TEST_CASE(tooltip_negative_delay_clamped_to_zero) {
    const TooltipNode tt("hi", -10.0F);
    AURORA_TEST_CHECK_NEAR(tt.delay_ms(), 0.0F, 0.0F);
    tt.hover_start(t_ms(0));
    tt.tick(t_ms(0));
    AURORA_TEST_CHECK_TRUE(tt.is_visible());
}

AURORA_TEST_CASE(context_menu_open_close_and_position) {
    int clicks = 0;
    std::vector<MenuItem> items;
    MenuItem entry;
    entry.label = "Copy";
    entry.on_click = [&clicks]() -> void { ++clicks; };
    items.push_back(entry);
    MenuItem sep;
    sep.separator = true;
    items.push_back(sep);

    const ContextMenuNode cm(items);
    AURORA_TEST_CHECK_EQ(cm.kind(), ModifierNode::Kind::Input);
    AURORA_TEST_CHECK_FALSE(cm.is_open());
    AURORA_TEST_REQUIRE_EQ(cm.items().size(), 2U);
    AURORA_TEST_CHECK_TRUE(cm.items()[1].separator);

    cm.open_at(Point{.x = 12.0F, .y = 34.0F});
    AURORA_TEST_CHECK_TRUE(cm.is_open());
    AURORA_TEST_CHECK_NEAR(cm.position().x, 12.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(cm.position().y, 34.0F, 0.0F);
    // 二次打开覆盖位置。
    cm.open_at(Point{.x = 50.0F, .y = 60.0F});
    AURORA_TEST_CHECK_NEAR(cm.position().x, 50.0F, 0.0F);
    cm.close();
    AURORA_TEST_CHECK_FALSE(cm.is_open());
}

AURORA_TEST_CASE(input_nodes_do_not_change_layout) {
    // 输入节点不改测量：约束透传、结果透传。
    const Clickable c([]() -> void {});
    const Constraints cons{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 80.0F, .height = 40.0F}};
    const Size s = c.layout(
        cons, [](const Constraints& cc) -> Size { return cc.constrain(Size{.width = 30.0F, .height = 20.0F}); });
    AURORA_TEST_CHECK_NEAR(s.width, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 20.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_modifier_input
