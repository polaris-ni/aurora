/// 测试类型: unit
/// 目标单元: include/aurora/widget/progress.h
/// 测试说明: 覆盖 ProgressIndicator——进度值 [0,1] 钳制、Reactive/Binding 双值来源与绑定回写、
/// collect_signals 随绑定扩展、厚度 setter 与布局高度、颜色 setter 与序列化往返、describe 元数据

#include <string>
#include <vector>

#include "aurora/state/binding.h"
#include "aurora/state/reactive.h"
#include "aurora/state/state.h"
#include "aurora/widget/progress.h"
#include "aurora/layout/layout_engine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_progress {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const ProgressIndicator p;
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "ProgressIndicator");
    AURORA_TEST_CHECK_NEAR(p.value(), 0.0, 1e-4);

    Json props;
    p.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["value"].get<double>(), 0.0, 1e-4);
    AURORA_TEST_CHECK_FALSE(props.contains("color"));  // 未显式设色不落盘，保留「跟随主题」语义
    AURORA_TEST_CHECK_EQ(props["track_color"][0].get<int>(), 220);
    AURORA_TEST_CHECK_NEAR(props["thickness"].get<float>(), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["corner_radius"].get<float>(), -1.0F, 1e-4F);  // <0 = 自动胶囊
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

AURORA_TEST_CASE(reactive_ctor_tracks_internal_value) {
    ProgressIndicator p{Reactive<double>{0.3}};
    AURORA_TEST_CHECK_NEAR(p.value(), 0.3, 1e-4);
    p.set_value(0.6);
    AURORA_TEST_CHECK_NEAR(p.value(), 0.6, 1e-4);
}

AURORA_TEST_CASE(binding_ctor_writes_back_to_upstream) {
    State<double> upstream{0.25};
    Binding<double> binding{upstream};
    ProgressIndicator p{binding};
    AURORA_TEST_CHECK_NEAR(p.value(), 0.25, 1e-4);  // 初值来自上游

    p.set_value(0.5);
    AURORA_TEST_CHECK_NEAR(upstream.get(), 0.5, 1e-4);  // 双向写回
    p.set_value(7.0);                                   // 钳制后再写入
    AURORA_TEST_CHECK_NEAR(upstream.get(), 1.0, 1e-4);
    AURORA_TEST_CHECK_NEAR(p.value(), 1.0, 1e-4);
}

AURORA_TEST_CASE(collect_signals_reflects_binding) {
    ProgressIndicator solo;
    std::vector<aurora::SignalViewBase *> solo_signals;
    solo.collect_signals(solo_signals);
    AURORA_TEST_CHECK_EQ(solo_signals.size(), 1U);  // 仅内部 value

    State<double> upstream{0.0};
    Binding<double> binding{upstream};
    ProgressIndicator bound{binding};
    std::vector<aurora::SignalViewBase *> bound_signals;
    bound.collect_signals(bound_signals);
    AURORA_TEST_CHECK_EQ(bound_signals.size(), 2U);  // value + 上游 State
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

    Json props;
    p.serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("color"));  // 显式设色后才落盘
    AURORA_TEST_CHECK_EQ(props["color"][2].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(props["track_color"][1].get<int>(), 6);
    AURORA_TEST_CHECK_NEAR(props["corner_radius"].get<float>(), 4.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    ProgressIndicator src;
    src.set_value(0.75);
    src.set_color(Color(9, 9, 9, 9)).set_track_color(Color(1, 2, 3, 4)).set_thickness(8.0F).set_corner_radius(0.0F);

    Json props;
    src.serialize_props(props);

    ProgressIndicator dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.value(), 0.75, 1e-4);
    LayoutEngine::layout(dst, bounded(150.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().height, 8.0F, 1e-4F);  // 反序列化的厚度生效

    Json out;
    dst.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(out["value"].get<double>(), 0.75, 1e-4);
    AURORA_TEST_CHECK_NEAR(out["thickness"].get<float>(), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out["corner_radius"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(out["color"][0].get<int>(), 9);
    AURORA_TEST_CHECK_EQ(out["track_color"][2].get<int>(), 3);

    // 反序列化路径同样钳制进度值。
    Json over = Json::object();
    over["value"] = 5.0;
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

}  // namespace aurora::test_cases::utest_progress
