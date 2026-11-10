/// 测试类型: unit
/// 目标单元: include/aurora/widget/radio_spin.h
/// 测试说明: 覆盖 RadioGroup——选项列表与初始索引钳制、select 边界（同值/越界不触发）、
/// 纵向点击选行、行列布局、序列化往返；以及 SpinBox——构造钳制、set_value/increment/decrement、
/// display_text 与上下方向键、序列化往返。两者的字号均未表态时取作用域主题的字阶正文档（RadioGroup
/// 另测主框度量随之变化），显式表态钉住两档作用域，序列化「只写意图」且非法值回填退出表态

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/radio_spin.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_radio_spin {
using aurora::testing::require_child;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// @param t 要注入的主题（按值拷进 holder 内的 Environment）。
/// @return holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
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
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "selected_index"), 1);
    AURORA_TEST_REQUIRE_EQ(require_child(props, "options")->size(), 2U);

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

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<double>(props, "value"), 50.0, 1e-4);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "decimals"), 1);

    SpinBox dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.value_of(), 50.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.min_value(), 0.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.max_value(), 100.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(dst.step(), 5.0, 1e-4);
    AURORA_TEST_CHECK_EQ(dst.display_text(), "$50.0 px");
}

// ---- 绘制（on_paint）覆盖 ----

AURORA_TEST_CASE(radiogroup_paint_vertical_horizontal_disabled_themed) {
    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 240.0F, .height = 120.0F}};

    // 纵向默态：覆盖 on_paint + paint_option（选中项内实心圆点 / 未选中仅圆环）。
    RadioGroup rg({"A", "B", "C"});
    LayoutEngine::layout(rg, bounded(240.0F, 120.0F));
    rg.paint(painter, Rect{.origin = Point{}, .size = rg.size()}, BuildContext{});

    // 水平排列：覆盖 on_paint 的横向累加分支。
    RadioGroup hor({"A", "B"}, 0, true);
    LayoutEngine::layout(hor, bounded(240.0F, 120.0F));
    hor.paint(painter, Rect{.origin = Point{}, .size = hor.size()}, BuildContext{});

    // 显式 active_color：覆盖 active_color_.value_or(...) 的「已设置」分支。
    RadioGroup themed({"A", "B"});
    themed.set_active_color(Color{10, 20, 30, 40});
    LayoutEngine::layout(themed, bounded(240.0F, 120.0F));
    themed.paint(painter, Rect{.origin = Point{}, .size = themed.size()}, BuildContext{});

    // 禁用态：覆盖 on_paint 灰化分支（accent/idle/text 统一回退灰）。
    RadioGroup off({"A", "B"});
    off.set_enabled(false);
    LayoutEngine::layout(off, bounded(240.0F, 120.0F));
    off.paint(painter, bounds, BuildContext{});

    // 选中项在末尾：覆盖纵向命中第 3 行 + 末项 paint_option 选中分支。
    RadioGroup sel_last({"A", "B", "C"});
    sel_last.select(2);
    LayoutEngine::layout(sel_last, bounded(240.0F, 120.0F));
    sel_last.paint(painter, Rect{.origin = Point{}, .size = sel_last.size()}, BuildContext{});
}

AURORA_TEST_CASE(spinbox_paint_rounded_square_disabled) {
    Painter painter;
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 120.0F, .height = 30.0F}};

    // 圆角（corner_radius 默认 4 > 0）：覆盖 paint_box 圆角分支 + paint_value + paint_arrows。
    SpinBox sb(50.0, 0.0, 100.0, 5.0);
    sb.set_prefix("$");
    sb.set_suffix(" px");
    sb.set_decimals(1);
    LayoutEngine::layout(sb, bounded(200.0F, 100.0F));
    sb.paint(painter, Rect{.origin = Point{}, .size = sb.size()}, BuildContext{});

    // 直角（corner_radius = 0）：覆盖 paint_box 直角分支。
    SpinBox sq(50.0, 0.0, 100.0, 5.0);
    sq.set_corner_radius(0.0F);
    LayoutEngine::layout(sq, bounded(200.0F, 100.0F));
    sq.paint(painter, Rect{.origin = Point{}, .size = sq.size()}, BuildContext{});

    // 禁用态：覆盖 on_paint 灰化分支（背景/边框/文本/箭头统一回退灰）。
    SpinBox off(50.0, 0.0, 100.0, 5.0);
    off.set_enabled(false);
    LayoutEngine::layout(off, bounded(200.0F, 100.0F));
    off.paint(painter, bounds, BuildContext{});
}

AURORA_TEST_CASE(font_size_follows_type_scale_when_unset) {
    Theme tuned;
    tuned.type_scale.body_medium.size_pt = 24.0F;
    const auto [env, ctx] = themed_ctx(tuned);
    (void)env;

    // ① RadioGroup 未表态 ⇒ 取作用域主题的字阶正文档（进程级 light 档 = Material 正文基准 14pt）。
    RadioGroup rg({"Alpha", "Beta"});
    AURORA_TEST_CHECK_NEAR(rg.effective_font_size(BuildContext{}), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rg.effective_font_size(ctx), 24.0F, 1e-4F);

    // ② 回落点确实进了度量路径：同一组选项，字阶调大 ⇒ 纵向行宽实测变宽。
    // 两个独立实例各排一次——同实例连排会命中布局缓存、整段跳过 `on_layout`（见 `Widget::layout`）。
    RadioGroup scaled({"Alpha", "Beta"});
    RadioGroup bare_layout({"Alpha", "Beta"});
    LayoutEngine::layout(scaled, bounded(600.0F, 300.0F), ctx);
    LayoutEngine::layout(bare_layout, bounded(600.0F, 300.0F));
    AURORA_TEST_CHECK_GT(scaled.size().width, bare_layout.size().width);

    // ③ 显式表态钉住：两档作用域都读显式值，主题覆盖不了它。
    rg.set_font_size(11.0F);
    AURORA_TEST_CHECK_NEAR(rg.effective_font_size(ctx), 11.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rg.effective_font_size(BuildContext{}), 11.0F, 1e-4F);

    // ④ 只写意图：表态过才落键，未表态不落键。
    Json stated = Json::object();
    rg.serialize_props(stated);
    AURORA_TEST_CHECK_NEAR(require_field<float>(stated, "font_size"), 11.0F, 1e-4F);
    Json bare = Json::object();
    RadioGroup{}.serialize_props(bare);
    AURORA_TEST_CHECK_FALSE(bare.contains("font_size"));

    // ⑤ SpinBox 同口径：未表态取字阶、表态钉住、非正字号退出表态且不落键。
    SpinBox sb(50.0, 0.0, 100.0, 5.0);
    AURORA_TEST_CHECK_NEAR(sb.effective_font_size(BuildContext{}), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sb.effective_font_size(ctx), 24.0F, 1e-4F);
    sb.set_font_size(11.0F);
    AURORA_TEST_CHECK_NEAR(sb.effective_font_size(ctx), 11.0F, 1e-4F);
    Json sb_stated = Json::object();
    sb.serialize_props(sb_stated);
    AURORA_TEST_CHECK_NEAR(require_field<float>(sb_stated, "font_size"), 11.0F, 1e-4F);
    sb.set_font_size(0.0F);
    Json sb_cleared = Json::object();
    sb.serialize_props(sb_cleared);
    AURORA_TEST_CHECK_FALSE(sb_cleared.contains("font_size"));
    AURORA_TEST_CHECK_NEAR(sb.effective_font_size(BuildContext{}), 14.0F, 1e-4F);

    // ⑥ 回填降级：非法值退出表态（交回主题，不退化成零高度文本）。
    SpinBox restored(50.0, 0.0, 100.0, 5.0);
    restored.deserialize_props(sb_stated);
    AURORA_TEST_CHECK_NEAR(restored.effective_font_size(BuildContext{}), 11.0F, 1e-4F);
    Json junk = Json::object();
    junk.set("font_size", Json{"not-a-number"});
    restored.deserialize_props(junk);
    AURORA_TEST_CHECK_NEAR(restored.effective_font_size(BuildContext{}), 14.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_radio_spin
