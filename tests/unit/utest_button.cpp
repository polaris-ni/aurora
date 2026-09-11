/// 测试类型: unit
/// 目标单元: include/aurora/widget/button.h
/// 测试说明: 覆盖 Button——默认属性、label 与 on_click 回调（activate 与 Press/Release 合成点击）、
/// 禁用态不触发回调、min_size 布局与约束钳制、自描述元数据、属性序列化往返

#include <string>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/button.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_button {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(button_defaults_and_type_name) {
    const Button b;
    AURORA_TEST_CHECK_EQ(std::string{b.type_name()}, "Button");
    const ButtonProps d = Button::defaults();
    AURORA_TEST_CHECK_TRUE(d.enabled);
    AURORA_TEST_CHECK_NEAR(d.corner_radius, 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_height, 0.0F, 1e-4F);
    // 默认背景色为 blue，文字色为 white。
    AURORA_TEST_CHECK_TRUE(d.color.get() == Color::blue());
    AURORA_TEST_CHECK_TRUE(d.on_color == Color::white());
}

AURORA_TEST_CASE(button_label_and_activate_fires_on_click) {
    int clicks = 0;
    Button b("OK");
    AURORA_TEST_CHECK_EQ(b.label.get().text, "OK");
    b.set_label("确认");
    AURORA_TEST_CHECK_EQ(b.label.get().text, "确认");

    b.set_on_click([&clicks]() -> void { ++clicks; });
    AURORA_TEST_CHECK_TRUE(b.wants_click());
    b.activate();
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 2);
}

AURORA_TEST_CASE(button_disabled_activate_does_not_fire) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });
    b.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(b.wants_click());
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 0);

    // 重新启用后恢复可点击。
    b.set_enabled(true);
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 1);
}

AURORA_TEST_CASE(button_disabled_swallows_pointer_events) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });
    b.set_enabled(false);

    MouseEvent press;
    press.action = MouseAction::Press;
    b.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    b.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);
    AURORA_TEST_CHECK_EQ(clicks, 0);
}

AURORA_TEST_CASE(button_pointer_press_release_fires_click_once) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });

    MouseEvent press;
    press.action = MouseAction::Press;
    b.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(clicks, 0);  // 按下不立即触发

    MouseEvent release;
    release.action = MouseAction::Release;
    b.on_pointer_event(release);
    AURORA_TEST_CHECK_EQ(clicks, 1);  // 完整按下-抬起触发一次
}

AURORA_TEST_CASE(button_layout_honors_min_size_and_clamps) {
    // 空标签 + 零内边距：自然尺寸为 0，被 min_size 撑到 (120, 48)。
    Button b("");
    b.set_padding(EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    b.set_min_size(120.0F, 48.0F);
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(b.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(b.size().height, 48.0F, 1e-4F);

    // 约束不足时钳制到 max。
    LayoutEngine::layout(b, bounded(50.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(b.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(b.size().height, 20.0F, 1e-4F);

    // 默认内边距下任意标签宽度至少含左右 padding（12+12）。
    Button c("OK");
    LayoutEngine::layout(c, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_GE(c.size().width, 24.0F);
    AURORA_TEST_CHECK_GE(c.size().height, 12.0F);
}

AURORA_TEST_CASE(button_describe_reports_metadata) {
    const auto d = Button::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Button");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_click");
    bool has_label = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "label") {
            has_label = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_label);
}

AURORA_TEST_CASE(button_serialize_deserialize_roundtrip) {
    Button src("确认");
    src.set_enabled(false);
    src.set_corner_radius(10.0F);
    src.set_min_size(80.0F, 36.0F);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["label"].get<std::string>(), "确认");
    AURORA_TEST_CHECK_EQ(props["enabled"].get<bool>(), false);

    Button dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.label.get().text, "确认");
    AURORA_TEST_CHECK_FALSE(dst.enabled);
    AURORA_TEST_CHECK_NEAR(dst.corner_radius, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.min_width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.min_height, 36.0F, 1e-4F);
}

AURORA_TEST_CASE(cursor_shape_hook_defaults_to_pointing_hand) {
    // I1：Button 悬停默认 PointingHand（控件级虚钩子）；修饰链显式声明在派发器解析时优先。
    Button b;
    AURORA_TEST_CHECK(b.cursor_shape() == std::optional{CursorShape::PointingHand});
}

}  // namespace aurora::test_cases::utest_button
