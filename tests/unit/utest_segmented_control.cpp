/// 测试类型: unit
/// 目标单元: include/aurora/widget/segmented_control.h
/// 测试说明: 覆盖 SegmentedControl——段列表与选中索引、set_selected、Press 命中分段触发 on_change
/// （含右键不响应、越界点击不切换）、禁用态忽略点击、布局尺寸、自描述、序列化往返

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/segmented_control.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_segmented_control {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(segmented_defaults_and_type_name) {
    const SegmentedControl sc({"Day", "Week", "Month"});
    AURORA_TEST_CHECK_EQ(std::string{sc.type_name()}, "SegmentedControl");
    AURORA_TEST_REQUIRE_EQ(sc.segments().size(), 3U);
    AURORA_TEST_CHECK_EQ(sc.segments()[0], "Day");
    AURORA_TEST_CHECK_EQ(sc.segments()[2], "Month");
    AURORA_TEST_CHECK_EQ(sc.selected(), 0);  // 默认选中首段
    AURORA_TEST_CHECK_TRUE(sc.enabled());
}

AURORA_TEST_CASE(segmented_set_selected) {
    SegmentedControl sc({"A", "B", "C"});
    sc.set_selected(2);
    AURORA_TEST_CHECK_EQ(sc.selected(), 2);
}

AURORA_TEST_CASE(segmented_press_selects_segment_and_fires_callback) {
    std::vector<int> seen;
    SegmentedControl sc({"Day", "Week", "Month"}, 1);
    sc.set_on_change([&seen](int i) -> void { seen.push_back(i); });

    // 命中首段（local x=0 落在第 1 段内）。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.0F, .y = 10.0F};
    sc.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(sc.selected(), 0);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen[0], 0);

    // 点击当前选中段：不重复触发回调。
    MouseEvent again;
    again.action = MouseAction::Press;
    again.button = MouseButton::Left;
    again.local_position = Point{.x = 0.0F, .y = 10.0F};
    sc.on_pointer_event(again);
    AURORA_TEST_CHECK_EQ(sc.selected(), 0);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
}

AURORA_TEST_CASE(segmented_right_button_ignored) {
    int calls = 0;
    SegmentedControl sc({"A", "B"}, 1);
    sc.set_on_change([&calls](int) -> void { ++calls; });

    MouseEvent right;
    right.action = MouseAction::Press;
    right.button = MouseButton::Right;
    right.local_position = Point{.x = 0.0F, .y = 10.0F};
    sc.on_pointer_event(right);
    AURORA_TEST_CHECK_FALSE(right.is_handled);  // 仅响应左键
    AURORA_TEST_CHECK_EQ(sc.selected(), 1);
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(segmented_press_beyond_segments_no_change) {
    int calls = 0;
    SegmentedControl sc({"A", "B", "C"});
    sc.set_on_change([&calls](int) -> void { ++calls; });

    MouseEvent far;
    far.action = MouseAction::Press;
    far.button = MouseButton::Left;
    far.local_position = Point{.x = 10000.0F, .y = 10.0F};
    sc.on_pointer_event(far);
    AURORA_TEST_CHECK_TRUE(far.is_handled);  // 按下即消费，但未命中任何段
    AURORA_TEST_CHECK_EQ(sc.selected(), 0);
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(segmented_disabled_ignores_clicks) {
    int calls = 0;
    SegmentedControl sc({"A", "B"}, 1);
    sc.set_on_change([&calls](int) -> void { ++calls; });
    sc.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(sc.enabled());

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.0F, .y = 10.0F};
    sc.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(sc.selected(), 1);
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(segmented_layout_sizes_positive) {
    SegmentedControl sc({"Day", "Week", "Month"});
    LayoutEngine::layout(sc, bounded(500.0F, 100.0F));
    AURORA_TEST_CHECK_GT(sc.size().width, 0.0F);  // 各段宽 = 文本 + 24
    AURORA_TEST_CHECK_GT(sc.size().height, 0.0F);  // 文本高 + 12
    AURORA_TEST_CHECK_LE(sc.size().width, 500.0F);
    AURORA_TEST_CHECK_LE(sc.size().height, 100.0F);
}

AURORA_TEST_CASE(segmented_describe_and_roundtrip) {
    const auto d = SegmentedControl::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "SegmentedControl");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");
    bool has_selected = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "selected") {
            has_selected = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_selected);

    SegmentedControl src({"A", "B", "C"}, 2);
    src.set_enabled(false);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["selected"].get<int>(), 2);
    AURORA_TEST_REQUIRE_EQ(props["segments"].size(), 3U);

    SegmentedControl dst;
    dst.deserialize_props(props);
    AURORA_TEST_REQUIRE_EQ(dst.segments().size(), 3U);
    AURORA_TEST_CHECK_EQ(dst.segments()[1], "B");
    AURORA_TEST_CHECK_EQ(dst.selected(), 2);
    AURORA_TEST_CHECK_FALSE(dst.enabled());
}

}  // namespace aurora::test_cases::utest_segmented_control
