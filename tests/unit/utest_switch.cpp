/// 测试类型: unit
/// 目标单元: include/aurora/widget/switch.h
/// 测试说明: 覆盖 Switch——默认态、set_value 触发 on_changed、Press/Release 合成点击翻转、
/// 禁用态忽略点击、共享 State 双向写穿、轨道尺寸布局（含非法值回退默认）、自描述、序列化往返

#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/switch.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_switch {
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 布局后绘制一帧（headless 直跑；inherit_theme 回退 Theme::light()）。
auto paint_once(Switch &s) -> void {
    LayoutEngine::layout(s, bounded(200.0F, 100.0F));
    Painter painter;
    s.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = s.size()}, BuildContext{});
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
    s.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

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
    s.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

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
    s.set_on_changed([&calls](bool) -> void { ++calls; });
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

AURORA_TEST_CASE(switch_shared_source_writes_through_to_upstream) {
    State<bool> up{true};
    Switch s{up};  // 共享同一来源：控件与上游是同一个值
    AURORA_TEST_CHECK_TRUE(s.value());

    s.set_value(false);
    AURORA_TEST_CHECK_FALSE(up.get());  // 控件 → 上游

    up.set(true);
    AURORA_TEST_CHECK_TRUE(s.value());  // 上游 → 控件（读取穿透）

    // 信号收集：共享来源只采集一次。
    StateSink out;
    s.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.sources().size(), std::size_t{1});
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

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "checked"), true);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "enabled"), false);

    Switch dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.value());
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    LayoutEngine::layout(dst, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 56.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 28.0F, 1e-4F);
}

AURORA_TEST_CASE(switch_paint_covers_state_branches) {
    // 开启态（显式主题色）+ 滑块右端。
    Switch on;
    on.set_value(true);
    on.set_active_color(Color{1, 2, 3, 4});
    on.set_thumb_color(Color{250, 250, 250, 255});
    paint_once(on);

    // 关闭态（默认灰轨道 + 无描边）。
    Switch off;
    paint_once(off);

    // 关闭态描边（enabled 时描边原色）。
    Switch bordered;
    bordered.set_border(Color{5, 6, 7, 8}, 1.5F);
    paint_once(bordered);

    // 关闭态描边 + 禁用（描边降透明分支）。
    Switch bordered_disabled;
    bordered_disabled.set_border(Color{5, 6, 7, 8}, 1.5F);
    bordered_disabled.set_enabled(false);
    paint_once(bordered_disabled);

    // 禁用态统一灰化（active/inactive/thumb 全走灰化分支）。
    Switch disabled;
    disabled.set_enabled(false);
    paint_once(disabled);

    // 悬停态轨道压暗（shaded 0.90 分支）。
    Switch hovered;
    hovered.on_hover_change(true);
    paint_once(hovered);

    // 按下态轨道压暗（shaded 0.80 分支；Press 置 pressed_）。
    Switch pressed;
    MouseEvent press;
    press.action = MouseAction::Press;
    pressed.on_pointer_event(press);
    paint_once(pressed);
}

AURORA_TEST_CASE(switch_accessibility_toggle_and_state) {
    Switch s;
    AURORA_TEST_CHECK_EQ(s.accessibility_value(), std::string{"false"});
    const AccessibilityState off_state = s.accessibility_state();
    AURORA_TEST_CHECK_TRUE(off_state.checkable);
    AURORA_TEST_CHECK_FALSE(off_state.checked);

    // 读屏 Toggle 动作翻转开关。
    const AccessibilityActionRequest req{.action = AccessibilityAction::Toggle};
    AURORA_TEST_CHECK_TRUE(s.perform_accessibility_action(req));
    AURORA_TEST_CHECK_TRUE(s.value());
    AURORA_TEST_CHECK_EQ(s.accessibility_value(), std::string{"true"});

    // 非 Toggle 动作回落基类（不翻转）。
    const AccessibilityActionRequest other{.action = AccessibilityAction::Focus};
    (void)s.perform_accessibility_action(other);
    AURORA_TEST_CHECK_TRUE(s.value());
}

AURORA_TEST_CASE(switch_deserialize_partial_and_thumb_inset) {
    // thumb_inset 非法值（<0）回落 2.0。
    Switch inset;
    inset.set_thumb_inset(-4.0F);
    paint_once(inset);

    // 反序列化部分键：只给 colors，不给尺寸/描边。
    Json props = Json::object();
    props.set("active_color", color_to_json(Color{9, 8, 7, 6}));
    props.set("inactive_color", color_to_json(Color{11, 12, 13, 14}));
    props.set("thumb_color", color_to_json(Color{15, 16, 17, 18}));
    Switch partial;
    partial.deserialize_props(props);
    paint_once(partial);
}

}  // namespace aurora::test_cases::utest_switch
