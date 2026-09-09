/// 测试类型: unit
/// 目标单元: include/aurora/widget/radio_spin.h
/// 测试说明: 覆盖 RadioGroup——选项列表与初始索引钳制、select 边界（同值/越界不触发）、
/// 纵向点击选行、行列布局、序列化往返；以及 SpinBox——构造钳制、set_value/increment/decrement、
/// display_text 与上下方向键、序列化往返

#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/radio_spin.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_radio_spin {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

// ---- RadioGroup ----

AURORA_TEST_CASE(radiogroup_defaults_and_type_name) {
    const RadioGroup rg({"A", "B", "C"});
    AURORA_TEST_CHECK_EQ(std::string{rg.type_name()}, "RadioGroup");
    AURORA_TEST_CHECK_EQ(rg.option_count(), 3U);
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 0);  // 默认选中首项
    AURORA_TEST_CHECK_TRUE(rg.enabled());
}

AURORA_TEST_CASE(radiogroup_select_and_callback_boundaries) {
    std::vector<int> seen;
    RadioGroup rg({"A", "B", "C"});
    rg.set_on_change([&seen](int i) -> void { seen.push_back(i); });

    rg.select(2);
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 2);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen[0], 2);

    rg.select(2);  // 同值重复选择：不触发回调
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);

    rg.select(-1);  // 负索引：忽略
    rg.select(99);  // 越界：忽略
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 2);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
}

AURORA_TEST_CASE(radiogroup_initial_index_clamped) {
    RadioGroup high({"A", "B"}, 5);
    AURORA_TEST_CHECK_EQ(high.selected_index(), 1);  // 钳到最后一项
    RadioGroup low({"A", "B"}, -3);
    AURORA_TEST_CHECK_EQ(low.selected_index(), 0);  // 钳到首项

    RadioGroup empty(std::vector<std::string>{});
    AURORA_TEST_CHECK_EQ(empty.option_count(), 0U);
    AURORA_TEST_CHECK_EQ(empty.selected_index(), 0);
    empty.select(0);  // 空选项组：选择被忽略
    AURORA_TEST_CHECK_EQ(empty.selected_index(), 0);
}

AURORA_TEST_CASE(radiogroup_pointer_press_selects_row) {
    std::vector<int> seen;
    RadioGroup rg({"A", "B", "C"});
    rg.set_on_change([&seen](int i) -> void { seen.push_back(i); });

    // 纵向行高 28：local y=35 落在第 2 行。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 30.0F, .y = 35.0F};
    rg.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 1);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen[0], 1);

    // 落在选项区之外（y=100 → 第 4 行，越界）：不切换。
    MouseEvent miss;
    miss.action = MouseAction::Press;
    miss.local_position = Point{.x = 30.0F, .y = 100.0F};
    rg.on_pointer_event(miss);
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 1);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);

    // 禁用态：吞掉事件但不切换。
    rg.set_enabled(false);
    MouseEvent off;
    off.action = MouseAction::Press;
    off.local_position = Point{.x = 30.0F, .y = 0.0F};
    rg.on_pointer_event(off);
    AURORA_TEST_CHECK_TRUE(off.is_handled);
    AURORA_TEST_CHECK_EQ(rg.selected_index(), 1);
    AURORA_TEST_CHECK_EQ(seen.size(), 1U);
}

AURORA_TEST_CASE(radiogroup_layout_rows_and_columns) {
    RadioGroup vertical({"A", "B", "C"});
    LayoutEngine::layout(vertical, bounded(500.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(vertical.size().height, 84.0F, 1e-4F);  // 3 × 28 行高
    AURORA_TEST_CHECK_GT(vertical.size().width, 0.0F);

    RadioGroup horizontal({"A", "B", "C"}, 0, true);
    LayoutEngine::layout(horizontal, bounded(500.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(horizontal.size().height, 28.0F, 1e-4F);
    AURORA_TEST_CHECK_GT(horizontal.size().width, 0.0F);
}

AURORA_TEST_CASE(radiogroup_describe_and_roundtrip) {
    const auto d = RadioGroup::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "RadioGroup");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");

    RadioGroup src({"A", "B"}, 1);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["selected_index"].get<int>(), 1);
    AURORA_TEST_REQUIRE_EQ(props["options"].size(), 2U);

    RadioGroup dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.option_count(), 2U);
    AURORA_TEST_CHECK_EQ(dst.selected_index(), 1);
}

// ---- SpinBox ----

AURORA_TEST_CASE(spinbox_ctor_clamps_range_and_step) {
    const SpinBox sb(50.0, 0.0, 100.0, 5.0);
    AURORA_TEST_CHECK_EQ(std::string{sb.type_name()}, "SpinBox");
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 50.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(sb.min_value(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(sb.max_value(), 100.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(sb.step(), 5.0, 1e-4);

    // 初始值钳到值域。
    const SpinBox over(150.0, 0.0, 100.0);
    AURORA_TEST_CHECK_NEAR(over.value_of(), 100.0, 1e-4);

    // max < min：max 退化为 min；step <= 0：退化为 1。
    const SpinBox swapped(0.0, 10.0, 5.0);
    AURORA_TEST_CHECK_NEAR(swapped.max_value(), 10.0, 1e-4);
    const SpinBox zero_step(0.0, 0.0, 10.0, 0.0);
    AURORA_TEST_CHECK_NEAR(zero_step.step(), 1.0, 1e-4);
}

AURORA_TEST_CASE(spinbox_set_value_increment_decrement) {
    std::vector<double> seen;
    SpinBox sb(50.0, 0.0, 100.0, 5.0);
    sb.set_on_change([&seen](double v) -> void { seen.push_back(v); });

    sb.increment();
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 55.0, 1e-4);
    sb.decrement();
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 50.0, 1e-4);

    sb.set_value(200.0);  // 钳到 max
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 100.0, 1e-4);
    sb.set_value(-1.0);  // 钳到 min
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 0.0, 1e-4);

    // 同值重复设置：不触发回调。
    sb.set_value(0.0);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 4U);
    AURORA_TEST_CHECK_NEAR(seen[0], 55.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(seen[1], 50.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(seen[2], 100.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(seen[3], 0.0, 1e-4);

    // 已在 max 时 increment 不再变化、不再回调。
    sb.set_value(100.0);
    sb.increment();
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 100.0, 1e-4);
    AURORA_TEST_CHECK_EQ(seen.size(), 5U);
}

AURORA_TEST_CASE(spinbox_display_text_and_arrow_keys) {
    SpinBox sb(50.0, 0.0, 100.0, 5.0);
    sb.set_prefix("$");
    sb.set_suffix(" px");
    sb.set_decimals(1);
    AURORA_TEST_CHECK_EQ(sb.display_text(), "$50.0 px");

    KeyEvent up;
    up.key = static_cast<int>(KeyCode::ArrowUp);
    up.action = KeyAction::Down;
    sb.on_key_event(up);
    AURORA_TEST_CHECK_TRUE(up.is_handled);
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 55.0, 1e-4);
    AURORA_TEST_CHECK_EQ(sb.display_text(), "$55.0 px");

    KeyEvent down;
    down.key = static_cast<int>(KeyCode::ArrowDown);
    down.action = KeyAction::Down;
    sb.on_key_event(down);
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 50.0, 1e-4);

    // 禁用态：方向键不调节。
    sb.set_enabled(false);
    KeyEvent blocked;
    blocked.key = static_cast<int>(KeyCode::ArrowUp);
    blocked.action = KeyAction::Down;
    sb.on_key_event(blocked);
    AURORA_TEST_CHECK_NEAR(sb.value_of(), 50.0, 1e-4);
}

AURORA_TEST_CASE(spinbox_describe_and_roundtrip) {
    const auto d = SpinBox::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "SpinBox");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_change");

    SpinBox src(50.0, 0.0, 100.0, 5.0);
    src.set_prefix("$");
    src.set_suffix(" px");
    src.set_decimals(1);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["value"].get<double>(), 50.0, 1e-4);
    AURORA_TEST_CHECK_EQ(props["decimals"].get<int>(), 1);

    SpinBox dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.value_of(), 50.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.min_value(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.max_value(), 100.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.step(), 5.0, 1e-4);
    AURORA_TEST_CHECK_EQ(dst.display_text(), "$50.0 px");
}

}  // namespace aurora::test_cases::utest_radio_spin
