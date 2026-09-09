/// 测试类型: unit
/// 目标单元: include/aurora/widget/checkbox.h
/// 测试说明: 覆盖 Checkbox——默认态、set_value 触发 on_changed、Press/Release 合成点击切换、
/// 禁用态忽略点击、Binding 双向写穿与信号收集、方框尺寸布局、自描述、序列化往返

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/state/state.h"
#include "aurora/widget/checkbox.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_checkbox {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(checkbox_defaults_and_type_name) {
    const Checkbox c;
    AURORA_TEST_CHECK_EQ(std::string{c.type_name()}, "Checkbox");
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_CHECK_TRUE(c.enabled());
}

AURORA_TEST_CASE(checkbox_set_value_fires_on_changed) {
    std::vector<bool> seen;
    Checkbox c;
    c.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

    c.set_value(true);
    AURORA_TEST_CHECK_TRUE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    c.set_value(false);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(checkbox_pointer_press_release_toggles) {
    std::vector<bool> seen;
    Checkbox c;
    c.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

    MouseEvent press;
    press.action = MouseAction::Press;
    c.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_FALSE(c.value());  // 按下不切换

    MouseEvent release;
    release.action = MouseAction::Release;
    c.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);
    AURORA_TEST_CHECK_TRUE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    // 第二次完整点击切回 false。
    MouseEvent press2;
    press2.action = MouseAction::Press;
    c.on_pointer_event(press2);
    MouseEvent release2;
    release2.action = MouseAction::Release;
    c.on_pointer_event(release2);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(checkbox_disabled_ignores_clicks) {
    int calls = 0;
    Checkbox c;
    c.set_on_changed([&calls](bool) -> void { ++calls; });
    c.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(c.enabled());

    MouseEvent press;
    press.action = MouseAction::Press;
    c.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    c.on_pointer_event(release);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(checkbox_binding_writes_through_to_upstream) {
    State<bool> up{false};
    Checkbox c{Binding<bool>(up)};
    AURORA_TEST_CHECK_FALSE(c.value());

    c.set_value(true);
    AURORA_TEST_CHECK_TRUE(up.get());  // 控件 → 上游

    up.set(false);
    AURORA_TEST_CHECK_FALSE(c.value());  // 上游 → 控件（读取穿透）

    // 信号收集：内部 value_ + 绑定目标。
    std::vector<SignalViewBase*> out;
    c.collect_signals(out);
    AURORA_TEST_REQUIRE_EQ(out.size(), 2U);

    // 非绑定构造只收集内部信号。
    Checkbox plain;
    std::vector<SignalViewBase*> single;
    plain.collect_signals(single);
    AURORA_TEST_CHECK_EQ(single.size(), 1U);
}

AURORA_TEST_CASE(checkbox_layout_uses_box_size) {
    Checkbox c;
    LayoutEngine::layout(c, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 20.0F, 1e-4F);

    c.set_size(30.0F);
    // 库运行时语义：Widget::layout 以约束为键做布局缓存（widget.cpp），set_size 不标脏，
    // 约束不变时命中缓存复用旧尺寸 20；换用不同约束强制真实重排。
    LayoutEngine::layout(c, bounded(210.0F, 110.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 30.0F, 1e-4F);

    // 约束不足时钳制。
    LayoutEngine::layout(c, bounded(25.0F, 25.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 25.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 25.0F, 1e-4F);
}

AURORA_TEST_CASE(checkbox_describe_reports_metadata) {
    const auto d = Checkbox::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Checkbox");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_changed");
    bool has_checked = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "checked") {
            has_checked = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_checked);
}

AURORA_TEST_CASE(checkbox_serialize_deserialize_roundtrip) {
    int calls = 0;
    Checkbox src;
    src.set_on_changed([&calls](bool) -> void { ++calls; });
    src.set_value(true);
    src.set_size(28.0F);
    src.set_enabled(false);
    AURORA_TEST_CHECK_EQ(calls, 1);  // 序列化前回调已随 set_value 触发

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["checked"].get<bool>(), true);
    AURORA_TEST_CHECK_EQ(props["enabled"].get<bool>(), false);

    Checkbox dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.value());
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    LayoutEngine::layout(dst, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 28.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_checkbox
