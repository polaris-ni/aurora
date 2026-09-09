/// 测试类型: unit
/// 目标单元: include/aurora/widget/slider.h
/// 测试说明: 覆盖 Slider——默认值域、set_value 钳制与 step 吸附、on_changed 回调、
/// Press/Move 合成拖拽按局部 x 映射值、禁用态忽略指针、Binding 写穿、自描述不变量、序列化往返

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/state/state.h"
#include "aurora/widget/slider.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_slider {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(slider_defaults_and_type_name) {
    const Slider s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Slider");
    AURORA_TEST_CHECK_NEAR(s.value(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(s.step(), 0.0, 1e-4);
    AURORA_TEST_CHECK_TRUE(s.enabled());
}

AURORA_TEST_CASE(slider_set_value_clamps_to_range) {
    Slider s;
    s.set_range(0.0, 10.0);
    s.set_value(99.0);
    AURORA_TEST_CHECK_NEAR(s.value(), 10.0, 1e-4);  // 上越界钳到 max
    s.set_value(-5.0);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.0, 1e-4);  // 下越界钳到 min
    s.set_value(4.25);
    AURORA_TEST_CHECK_NEAR(s.value(), 4.25, 1e-4);
}

AURORA_TEST_CASE(slider_step_snaps_to_grid) {
    Slider s;
    s.set_range(0.0, 10.0);
    s.set_step(2.0);
    AURORA_TEST_CHECK_NEAR(s.step(), 2.0, 1e-4);

    s.set_value(3.3);  // round(3.3/2)=2 → 4.0
    AURORA_TEST_CHECK_NEAR(s.value(), 4.0, 1e-4);

    s.set_value(0.9);  // round(0.9/2)=0 → 0.0
    AURORA_TEST_CHECK_NEAR(s.value(), 0.0, 1e-4);

    // step 归零恢复连续无级。
    s.set_step(0.0);
    s.set_value(3.3);
    AURORA_TEST_CHECK_NEAR(s.value(), 3.3, 1e-4);
}

AURORA_TEST_CASE(slider_on_changed_receives_clamped_value) {
    std::vector<double> seen;
    Slider s;
    s.set_range(0.0, 1.0);
    s.set_on_changed([&seen](double v) { seen.push_back(v); });

    s.set_value(0.5);
    s.set_value(7.0);  // 钳到 1.0 后再上报
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_NEAR(seen[0], 0.5, 1e-4);
    AURORA_TEST_CHECK_NEAR(seen[1], 1.0, 1e-4);
}

AURORA_TEST_CASE(slider_drag_maps_local_x_to_value) {
    std::vector<double> seen;
    Slider s;
    s.set_on_changed([&seen](double v) { seen.push_back(v); });
    LayoutEngine::layout(s, bounded(200.0F, 24.0F));  // 宽 200 → 轨道 192（左右各缩 4）

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 100.0F, .y = 12.0F};  // (100-4)/192 = 0.5
    s.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.5, 1e-4);

    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = 148.0F, .y = 12.0F};  // (148-4)/192 = 0.75
    s.on_pointer_event(move);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.75, 1e-4);

    MouseEvent release;
    release.action = MouseAction::Release;
    s.on_pointer_event(release);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.75, 1e-4);  // 松手不改值
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
}

AURORA_TEST_CASE(slider_disabled_ignores_pointer_events) {
    int calls = 0;
    Slider s;
    s.set_range(0.0, 1.0);
    s.set_on_changed([&calls](double) { ++calls; });
    LayoutEngine::layout(s, bounded(200.0F, 24.0F));
    s.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(s.enabled());

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 100.0F, .y = 12.0F};
    s.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.0, 1e-4);
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(slider_binding_writes_through_to_upstream) {
    State<double> up{0.0};
    Slider s{Binding<double>(up)};
    s.set_value(0.7);
    AURORA_TEST_CHECK_NEAR(up.get(), 0.7, 1e-4);  // 控件 → 上游

    up.set(0.2);
    AURORA_TEST_CHECK_NEAR(s.value(), 0.2, 1e-4);  // 上游 → 控件（读取穿透）

    // 信号收集：内部 value_ + 绑定目标。
    std::vector<SignalViewBase *> out;
    s.collect_signals(out);
    AURORA_TEST_REQUIRE_EQ(out.size(), 2U);
}

AURORA_TEST_CASE(slider_describe_reports_invariants) {
    const auto d = Slider::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Slider");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_changed");
    AURORA_TEST_REQUIRE_EQ(d.invariants.size(), 3U);
    AURORA_TEST_CHECK_EQ(std::string{d.invariants[0]}, "min <= max");
}

AURORA_TEST_CASE(slider_serialize_deserialize_roundtrip) {
    Slider src;
    src.set_range(0.0, 100.0);
    src.set_value(42.0);
    src.set_enabled(false);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["value"].get<double>(), 42.0, 1e-4);
    AURORA_TEST_CHECK_EQ(props["enabled"].get<bool>(), false);

    Slider dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.value(), 42.0, 1e-4);
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    LayoutEngine::layout(dst, bounded(300.0F, 24.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 300.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_slider
