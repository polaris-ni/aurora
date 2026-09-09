/// 测试类型: unit
/// 目标单元: include/aurora/widget/switch.h
/// 测试说明: 覆盖 Switch——默认态、set_value 触发 on_changed、Press/Release 合成点击翻转、
/// 禁用态忽略点击、Binding 双向写穿、轨道尺寸布局（含非法值回退默认）、自描述、序列化往返

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/state/state.h"
#include "aurora/widget/switch.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_switch {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(switch_defaults_and_type_name) {
    const Switch s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Switch");
    AURORA_TEST_CHECK_FALSE(s.value());
    AURORA_TEST_CHECK_TRUE(s.enabled());
}

AURORA_TEST_CASE(switch_set_value_fires_on_changed) {
    std::vector<bool> seen;
    Switch s;
    s.set_on_changed([&seen](bool v) { seen.push_back(v); });

    s.set_value(true);
    AURORA_TEST_CHECK_TRUE(s.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    s.set_value(false);
    AURORA_TEST_CHECK_FALSE(s.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(switch_pointer_press_release_toggles) {
    std::vector<bool> seen;
    Switch s;
    s.set_on_changed([&seen](bool v) { seen.push_back(v); });

    MouseEvent press;
    press.action = MouseAction::Press;
    s.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_FALSE(s.value());  // 按下不翻转

    MouseEvent release;
    release.action = MouseAction::Release;
    s.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);
    AURORA_TEST_CHECK_TRUE(s.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    // 再翻转一次回 off。
    MouseEvent press2;
    press2.action = MouseAction::Press;
    s.on_pointer_event(press2);
    MouseEvent release2;
    release2.action = MouseAction::Release;
    s.on_pointer_event(release2);
    AURORA_TEST_CHECK_FALSE(s.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(switch_disabled_ignores_clicks) {
    int calls = 0;
    Switch s;
    s.set_on_changed([&calls](bool) { ++calls; });
    s.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(s.enabled());

    MouseEvent press;
    press.action = MouseAction::Press;
    s.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    s.on_pointer_event(release);
    AURORA_TEST_CHECK_FALSE(s.value());
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(switch_binding_writes_through_to_upstream) {
    State<bool> up{true};
    Switch s{Binding<bool>(up)};
    AURORA_TEST_CHECK_TRUE(s.value());

    s.set_value(false);
    AURORA_TEST_CHECK_FALSE(up.get());  // 控件 → 上游

    up.set(true);
    AURORA_TEST_CHECK_TRUE(s.value());  // 上游 → 控件（读取穿透）

    // 信号收集：内部 value_ + 绑定目标。
    std::vector<SignalViewBase *> out;
    s.collect_signals(out);
    AURORA_TEST_REQUIRE_EQ(out.size(), 2U);
}

AURORA_TEST_CASE(switch_layout_uses_track_size) {
    Switch s;
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 44.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 24.0F, 1e-4F);

    s.set_track_size(60.0F, 30.0F);
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 30.0F, 1e-4F);

    // 非法（<=0）尺寸回退默认轨道尺寸。
    s.set_track_size(0.0F, 0.0F);
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 44.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 24.0F, 1e-4F);

    // 约束不足时钳制。
    s.set_track_size(60.0F, 30.0F);
    LayoutEngine::layout(s, bounded(50.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(switch_describe_reports_metadata) {
    const auto d = Switch::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Switch");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_changed");
    bool has_checked = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "checked") {
            has_checked = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_checked);
}

AURORA_TEST_CASE(switch_serialize_deserialize_roundtrip) {
    Switch src;
    src.set_value(true);
    src.set_track_size(56.0F, 28.0F);
    src.set_enabled(false);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["checked"].get<bool>(), true);
    AURORA_TEST_CHECK_EQ(props["enabled"].get<bool>(), false);

    Switch dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.value());
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    LayoutEngine::layout(dst, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 56.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 28.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_switch
