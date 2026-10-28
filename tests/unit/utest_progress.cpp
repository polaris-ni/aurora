/// 测试类型: unit
/// 目标单元: include/aurora/widget/progress.h
/// 测试说明: 覆盖 ProgressIndicator——进度值 [0,1] 钳制、自有/外部只读来源与挂载换源重订阅、
/// collect_signals 随有效来源切换、厚度 setter 与布局高度、颜色 setter 与序列化往返、describe 元数据

#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/progress.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_progress {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const ProgressIndicator p;
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "ProgressIndicator");
    AURORA_TEST_CHECK_NEAR(p.value(), 0.0, 1e-4);

    Json props = Json::object();
    p.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<double>(props, "value"), 0.0, 1e-4);
    AURORA_TEST_CHECK_FALSE(props.contains("color"));  // 未显式设色不落盘，保留「跟随主题」语义
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "track_color"), 0)->as_or<int>(0), 220);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "thickness"), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "corner_radius"), -1.0F, 1e-4F);  // <0 = 自动胶囊
}

AURORA_TEST_CASE(set_value_clamps_to_unit_range) {
    ProgressIndicator p;
    p.set_value(0.42);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.42, 1e-4);
    p.set_value(2.5);  // 上界钳制
    AURORA_TEST_CHECK_NEAR(p.value(), 1.0, 1e-4);
    p.set_value(-1.0);  // 下界钳制
    AURORA_TEST_CHECK_NEAR(p.value(), 0.0, 1e-4);
}

AURORA_TEST_CASE(literal_ctor_tracks_own_value) {
    ProgressIndicator p{0.3};
    AURORA_TEST_CHECK_NEAR(p.value(), 0.3, 1e-4);
    p.set_value(0.6);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.6, 1e-4);
}

AURORA_TEST_CASE(external_source_tracks_upstream_without_writing_back) {
    // 纯展示件：外部只读来源随上游变化；set_value 换回自有来源，不代写上游。
    State<double> upstream{0.25};
    ProgressIndicator p{upstream};
    AURORA_TEST_CHECK_NEAR(p.value(), 0.25, 1e-4);  // 初值来自上游

    upstream.set(0.5);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.5, 1e-4);  // 上游 → 控件（读取穿透）

    p.set_value(1.5);  // 展示件换源到自有：钳制到 1.0
    AURORA_TEST_CHECK_NEAR(p.value(), 1.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(upstream.get(), 0.5, 1e-4);  // 上游不被代写
}

AURORA_TEST_CASE(collect_signals_reflects_active_source) {
    // 采集出口只登记当前有效来源：外部只读或自有，二者不会同时出现。
    ProgressIndicator solo;
    StateSink solo_signals;
    solo.collect_signals(solo_signals);
    AURORA_TEST_CHECK_EQ(solo_signals.sources().size(), std::size_t{1});  // 仅内部来源

    State<double> upstream{0.0};
    ProgressIndicator bound{upstream};
    StateSink bound_signals;
    bound.collect_signals(bound_signals);
    AURORA_TEST_CHECK_EQ(bound_signals.sources().size(), std::size_t{1});  // 仅外部来源
}

AURORA_TEST_CASE(mounted_swap_source_resubscribes_old_and_new) {
    // 挂载后换源必须「退旧 + 订新」：A、B 初值相等，换源不能靠值比较判得失。
    State<double> a{0.5};
    State<double> b{0.5};
    ProgressIndicator p{a};
    BuildContext ctx;
    p.mount(ctx);

    int dirty = 0;
    p.on_dirty = [&dirty](bool /*layout*/) -> void { ++dirty; };

    a.set(0.7);  // 挂载订阅的是 A
    AURORA_TEST_CHECK_TRUE(dirty > 0);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.7, 1e-4);

    p.set_value_source(b);  // 等值 A→B 换源（本调用自身标脏一次）
    AURORA_TEST_CHECK_NEAR(p.value(), 0.5, 1e-4);

    dirty = 0;
    a.set(0.9);  // 旧源必须已退订
    AURORA_TEST_CHECK_EQ(dirty, 0);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.5, 1e-4);  // 旧源写入不影响读值

    b.set(0.2);  // 新源必须已订阅
    AURORA_TEST_CHECK_TRUE(dirty > 0);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.2, 1e-4);

    p.unmount();
    dirty = 0;
    b.set(0.3);  // 卸载即退订
    AURORA_TEST_CHECK_EQ(dirty, 0);

    p.mount(ctx);  // 重挂按当前有效来源（B）重建订阅
    dirty = 0;
    b.set(0.4);
    AURORA_TEST_CHECK_TRUE(dirty > 0);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.4, 1e-4);
    p.unmount();
}

AURORA_TEST_CASE(thickness_setter_and_layout_height) {
    ProgressIndicator p;
    p.set_thickness(10.0F);
    LayoutEngine::layout(p, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(p.size().height, 10.0F, 1e-4F);  // 厚度决定自然高度
    AURORA_TEST_CHECK_NEAR(p.size().width, 200.0F, 1e-4F);  // 宽度撑满约束

    ProgressIndicator degenerate;
    degenerate.set_thickness(-1.0F);  // 非正厚度回落默认 6
    LayoutEngine::layout(degenerate, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(degenerate.size().height, 6.0F, 1e-4F);
}

AURORA_TEST_CASE(color_setters_drive_serialization) {
    ProgressIndicator p;
    p.set_color(Color(1, 2, 3, 4)).set_track_color(Color(5, 6, 7, 8)).set_corner_radius(4.0F);

    Json props = Json::object();
    p.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("color"));  // 显式设色后才落盘
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "color"), 2)->as_or<int>(0), 3);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "track_color"), 1)->as_or<int>(0), 6);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "corner_radius"), 4.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    ProgressIndicator src;
    src.set_value(0.75);
    src.set_color(Color(9, 9, 9, 9)).set_track_color(Color(1, 2, 3, 4)).set_thickness(8.0F).set_corner_radius(0.0F);

    Json props = Json::object();
    src.serialize_props(props);

    ProgressIndicator dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.value(), 0.75, 1e-4);
    LayoutEngine::layout(dst, bounded(150.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().height, 8.0F, 1e-4F);  // 反序列化的厚度生效

    Json out = Json::object();
    dst.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(require_field<double>(out, "value"), 0.75, 1e-4);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "thickness"), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "corner_radius"), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "color"), 0)->as_or<int>(0), 9);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "track_color"), 2)->as_or<int>(0), 3);

    // 反序列化路径同样钳制进度值。
    Json over = Json::object();
    over.set("value", 5.0);
    ProgressIndicator clamped;
    clamped.deserialize_props(over);
    AURORA_TEST_CHECK_NEAR(clamped.value(), 1.0, 1e-4);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = ProgressIndicator::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "ProgressIndicator");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_CHECK_TRUE(!d.invariants.empty());  // 声明值域/厚度不变量
    bool has_value = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "value") {
            has_value = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_value);
}

AURORA_TEST_CASE(paint_covers_capsule_rect_and_rounded) {
    Painter painter;

    // 默认：corner_radius < 0 → 自动胶囊路径。
    ProgressIndicator def;
    def.set_value(0.5);
    LayoutEngine::layout(def, bounded(200.0F, 100.0F));
    def.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = def.size()}, BuildContext{});

    // 直角（corner_radius = 0）路径。
    ProgressIndicator square;
    square.set_value(0.3);
    square.set_corner_radius(0.0F);
    LayoutEngine::layout(square, bounded(200.0F, 100.0F));
    square.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = square.size()}, BuildContext{});

    // 圆角（corner_radius > 0）路径 + 显式颜色。
    ProgressIndicator rounded;
    rounded.set_value(0.8);
    rounded.set_color(Color(1, 2, 3, 4));
    rounded.set_corner_radius(4.0F);
    LayoutEngine::layout(rounded, bounded(200.0F, 100.0F));
    rounded.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = rounded.size()}, BuildContext{});
}

}  // namespace aurora::test_cases::utest_progress
