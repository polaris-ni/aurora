/// 测试类型: unit
/// 目标单元: include/aurora/event/event.h
/// 测试说明: 各事件结构默认值与 is_handled 标志、ModifierKey 位组合算子、同类型拷贝与多态析构、TouchEvent
/// 活跃触点统计/按 id 查找/双指几何

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "aurora/event/event.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_event {

AURORA_TEST_CASE(event_defaults_and_handled_flag) {
    const Event base{};
    AURORA_TEST_CHECK_FALSE(base.is_handled);

    const MouseEvent mouse{};
    AURORA_TEST_CHECK_FALSE(mouse.is_handled);
    AURORA_TEST_CHECK_NEAR(mouse.position.x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.position.y, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.local_position.x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(mouse.local_position.y, 0.0F, 1e-6F);
    AURORA_TEST_CHECK(mouse.button == MouseButton::Left);
    AURORA_TEST_CHECK(mouse.action == MouseAction::Press);
    AURORA_TEST_CHECK_FALSE(mouse.pointer_id.has_value());

    const KeyEvent key{};
    AURORA_TEST_CHECK_EQ(key.key, 0);
    AURORA_TEST_CHECK(key.action == KeyAction::Down);
    AURORA_TEST_CHECK(key.modifiers == ModifierKey::None);

    const ScrollEvent scroll{};
    AURORA_TEST_CHECK_EQ(scroll.delta_x, 0.0F);
    AURORA_TEST_CHECK_EQ(scroll.delta_y, 0.0F);

    const TextInputEvent text{};
    AURORA_TEST_CHECK(text.text.empty());

    const FileDropEvent drop{};
    AURORA_TEST_CHECK(drop.paths.empty());

    const TouchPoint point{};
    AURORA_TEST_CHECK_EQ(point.id, 0);
    AURORA_TEST_CHECK_TRUE(point.is_active);

    const TouchEvent touch{};
    AURORA_TEST_CHECK_EQ(touch.active_count(), 0);
    AURORA_TEST_CHECK_EQ(touch.pinch_distance(), 0.0F);  // 少于 2 活跃触点按约定返回 0
    AURORA_TEST_CHECK_EQ(touch.pinch_angle(), 0.0F);
    AURORA_TEST_CHECK_FALSE(touch.point_by_id(0).has_value());
}

AURORA_TEST_CASE(modifier_key_bitwise_or_and_and) {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::None), std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Shift), std::uint8_t{1});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Control), std::uint8_t{2});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Alt), std::uint8_t{4});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::Meta), std::uint8_t{8});

    // operator| 组合位掩码；operator& 返回 uint8_t 便于判位
    const auto combo = ModifierKey::Shift | ModifierKey::Control;
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(combo), std::uint8_t{3});
    AURORA_TEST_CHECK_NE(combo & ModifierKey::Shift, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(combo & ModifierKey::Control, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(combo & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(combo | ModifierKey::Shift), std::uint8_t{3});  // 幂等
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierKey::None | ModifierKey::Meta), std::uint8_t{8});

    // 与 KeyEvent.modifiers 字段的往返用法
    KeyEvent e;
    e.modifiers = ModifierKey::Alt | ModifierKey::Meta;
    AURORA_TEST_CHECK_NE(e.modifiers & ModifierKey::Alt, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(e.modifiers & ModifierKey::Shift, std::uint8_t{0});
}

AURORA_TEST_CASE(same_type_copy_preserves_event_payload) {
    MouseEvent press;
    press.position = Point{.x = 12.0F, .y = 34.0F};
    press.button = MouseButton::Right;
    press.action = MouseAction::Release;
    press.pointer_id = 7;
    press.is_handled = true;

    MouseEvent copied = press;
    AURORA_TEST_CHECK_NEAR(copied.position.x, 12.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(copied.position.y, 34.0F, 1e-6F);
    AURORA_TEST_CHECK(copied.button == MouseButton::Right);
    AURORA_TEST_CHECK(copied.action == MouseAction::Release);
    AURORA_TEST_REQUIRE(copied.pointer_id.has_value());
    AURORA_TEST_CHECK_EQ(copied.pointer_id.value(), 7);
    AURORA_TEST_CHECK_TRUE(copied.is_handled);

    // 改副本不影响原件
    copied.is_handled = false;
    copied.pointer_id.reset();
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_TRUE(press.pointer_id.has_value());

    // 触摸事件：points 向量随拷贝独立
    TouchEvent with_points;
    with_points.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 1.0F, .y = 1.0F}});
    TouchEvent touch_copy = with_points;
    AURORA_TEST_CHECK_EQ(touch_copy.points.size(), std::size_t{1});
    touch_copy.points.clear();
    AURORA_TEST_CHECK_EQ(with_points.points.size(), std::size_t{1});

    // 经基类指针多态销毁（虚析构）
    std::unique_ptr<Event> polymorphic = std::make_unique<ScrollEvent>();
    AURORA_TEST_CHECK_NO_THROW(polymorphic.reset());
}

AURORA_TEST_CASE(touch_active_count_and_point_lookup) {
    TouchEvent e;
    e.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}});
    e.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 5.0F, .y = 0.0F}, .is_active = false});
    e.points.push_back(TouchPoint{.id = 3, .position = Point{.x = 9.0F, .y = 9.0F}});

    AURORA_TEST_CHECK_EQ(e.active_count(), 2);

    const auto hit = e.point_by_id(2);
    AURORA_TEST_REQUIRE(hit.has_value());
    // value() 替代 operator->：tidy 无法识别宏内 has_value 断言，value() 空时抛出、失败信息更清晰。
    // 前序 AURORA_TEST_REQUIRE 已保证 has_value，tidy 无法穿透断言宏的 CFG，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(hit.value().id, 2);
    AURORA_TEST_CHECK_FALSE(hit.value().is_active);
    AURORA_TEST_CHECK_NEAR(hit.value().position.x, 5.0F, 1e-6F);
    // NOLINTEND(bugprone-unchecked-optional-access)

    AURORA_TEST_CHECK_FALSE(e.point_by_id(99).has_value());
}

AURORA_TEST_CASE(touch_pinch_distance_and_angle) {
    TouchEvent e;
    e.points.push_back(TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}});
    e.points.push_back(TouchPoint{.id = 2, .position = Point{.x = 3.0F, .y = 4.0F}});
    AURORA_TEST_CHECK_NEAR(e.pinch_distance(), 5.0F, 1e-4F);  // 3-4-5 直角三角形
    AURORA_TEST_CHECK_NEAR(e.pinch_angle(), 0.9272952F, 1e-4F);  // atan2(4, 3)

    // 仅剩 1 个活跃触点：几何查询按约定返回 0
    e.points[1].is_active = false;
    AURORA_TEST_CHECK_EQ(e.active_count(), 1);
    AURORA_TEST_CHECK_EQ(e.pinch_distance(), 0.0F);
    AURORA_TEST_CHECK_EQ(e.pinch_angle(), 0.0F);
}

AURORA_TEST_CASE(base_reference_shares_handled_flag) {
    // 派发器经由 Event& 写 is_handled 停止冒泡：基类引用与派生对象共享同一标志
    KeyEvent key;
    Event& base = key;
    AURORA_TEST_CHECK_FALSE(base.is_handled);
    base.is_handled = true;
    AURORA_TEST_CHECK_TRUE(key.is_handled);

    MouseEvent mouse;
    Event& mouse_base = mouse;
    mouse_base.is_handled = true;
    AURORA_TEST_CHECK_TRUE(mouse.is_handled);
}

}  // namespace aurora::test_cases::utest_event
