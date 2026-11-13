/// 测试类型: unit
/// 目标单元: include/aurora/widget/segmented_control.h
/// 测试说明: 覆盖 SegmentedControl——段列表与选中索引、set_selected、Press 命中分段触发 on_change
/// （含右键不响应、越界点击不切换）、禁用态忽略点击、布局尺寸、自描述、序列化往返；另有字号
/// 未表态时取主题字阶正文档（含命中区与量宽同源）、显式表态钉住与「只写意图」序列化的成对断言

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/segmented_control.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_segmented_control {
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
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "selected") {
            has_selected = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_selected);

    SegmentedControl src({"A", "B", "C"}, 2);
    src.set_enabled(false);
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "selected"), 2);
    AURORA_TEST_REQUIRE_EQ(require_child(props, "segments")->size(), 3U);

    SegmentedControl dst;
    dst.deserialize_props(props);
    AURORA_TEST_REQUIRE_EQ(dst.segments().size(), 3U);
    AURORA_TEST_CHECK_EQ(dst.segments()[1], "B");
    AURORA_TEST_CHECK_EQ(dst.selected(), 2);
    AURORA_TEST_CHECK_FALSE(dst.enabled());
}

AURORA_TEST_CASE(paint_covers_theme_clip_and_disabled_branches) {
    Painter painter;

    // 显式 active_color + 直角外框（draw_rect 分支）。
    SegmentedControl sc({"A", "B", "C"}, 1);
    sc.set_active_color(Color{10, 20, 30, 255});
    LayoutEngine::layout(sc, bounded(400.0F, 100.0F));
    Rect const bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = sc.size()};
    sc.paint(painter, bounds, BuildContext{});

    // 圆角路径：push/pop clip + draw_rounded_border。
    SegmentedControl rounded({"X", "Y"});
    rounded.set_corner_radius(8.0F);
    LayoutEngine::layout(rounded, bounded(400.0F, 100.0F));
    Rect const rb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = rounded.size()};
    rounded.paint(painter, rb, BuildContext{});

    // 禁用态灰化分支。
    SegmentedControl dis({"P", "Q"});
    dis.set_enabled(false);
    LayoutEngine::layout(dis, bounded(400.0F, 100.0F));
    Rect const db{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = dis.size()};
    dis.paint(painter, db, BuildContext{});

    // 默认主题：active_color 空 → inherit_theme(ctx).primary。
    SegmentedControl def({"M", "N"});
    LayoutEngine::layout(def, bounded(400.0F, 100.0F));
    Rect const fb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = def.size()};
    def.paint(painter, fb, BuildContext{});
}

AURORA_TEST_CASE(font_size_follows_type_scale_when_unset) {
    Theme tuned;
    tuned.type_scale.body_medium.size_pt = 24.0F;
    const auto [env, ctx] = themed_ctx(tuned);
    (void)env;

    SegmentedControl sc({"Alpha", "WWWWWWWWWW"});

    // ① 未表态 ⇒ 取作用域主题的字阶正文档（进程级 light 档 = Material 正文基准 14pt）。
    AURORA_TEST_CHECK_NEAR(sc.effective_font_size(BuildContext{}), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sc.effective_font_size(ctx), 24.0F, 1e-4F);

    // ② 回落点进了度量路径：字阶调大 ⇒ 分段条实测变宽。
    // 两个独立实例各排一次——同实例连排会命中布局缓存、整段跳过 `on_layout`（见 `Widget::layout`）。
    SegmentedControl scaled({"Alpha", "WWWWWWWWWW"});
    SegmentedControl bare({"Alpha", "WWWWWWWWWW"});
    LayoutEngine::layout(scaled, bounded(900.0F, 200.0F), ctx);
    LayoutEngine::layout(bare, bounded(900.0F, 200.0F));
    AURORA_TEST_CHECK_GT(scaled.size().width, bare.size().width);

    // ③ 命中判定与量宽同源：取点落在「14pt 首段之外、24pt 首段之内」。
    // 第二段取宽文本，保证该点在 14pt 档稳落在第二段跨度内 ⇒ 同一点两档命中不同段才说明命中区随字阶走
    // （贴着 60dp 下限或跨度边界取点，会因末点开区间而误判成「未命中」）。
    const float w14 = render::FontEngine::measure_width("Alpha", Font{.size_pt = 14.0F}) + 24.0F;
    const float w24 = render::FontEngine::measure_width("Alpha", Font{.size_pt = 24.0F}) + 24.0F;
    const float second14 = render::FontEngine::measure_width("WWWWWWWWWW", Font{.size_pt = 14.0F}) + 24.0F;
    const float probe_x = w14 + ((w24 - w14) * 0.5F);
    AURORA_TEST_REQUIRE_GT(w24, w14);
    AURORA_TEST_REQUIRE_LT(probe_x, w14 + second14);

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = probe_x, .y = 10.0F};
    scaled.set_selected(1);
    scaled.on_pointer_event(press);
    AURORA_TEST_CHECK_EQ(scaled.selected(), 0);  // 24pt 档：该点仍在首段内 → 切回首段

    MouseEvent same;
    same.action = MouseAction::Press;
    same.button = MouseButton::Left;
    same.local_position = Point{.x = probe_x, .y = 10.0F};
    bare.on_pointer_event(same);
    AURORA_TEST_CHECK_EQ(bare.selected(), 1);  // 14pt 档：同一点已过首段 → 命中第二段

    // ④ 显式表态钉住：两档作用域都读显式值，主题覆盖不了它。
    sc.set_font_size(11.0F);
    AURORA_TEST_CHECK_NEAR(sc.effective_font_size(ctx), 11.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sc.effective_font_size(BuildContext{}), 11.0F, 1e-4F);

    // ⑤ 只写意图 + 回填降级：表态过才落键；非法值不落键且退出表态（回落点交回主题）。
    Json stated = Json::object();
    sc.serialize_props(stated);
    AURORA_TEST_CHECK_NEAR(require_field<float>(stated, "font_size"), 11.0F, 1e-4F);
    Json bare_props = Json::object();
    SegmentedControl(std::vector<std::string>{"A"}).serialize_props(bare_props);
    AURORA_TEST_CHECK_FALSE(bare_props.contains("font_size"));
    Json junk = Json::object();
    junk.set("font_size", Json{"not-a-number"});
    sc.deserialize_props(junk);
    AURORA_TEST_CHECK_NEAR(sc.effective_font_size(BuildContext{}), 14.0F, 1e-4F);
    Json cleared = Json::object();
    sc.serialize_props(cleared);
    AURORA_TEST_CHECK_FALSE(cleared.contains("font_size"));  // 对照：非法值确实清了表态
}

}  // namespace aurora::test_cases::utest_segmented_control
