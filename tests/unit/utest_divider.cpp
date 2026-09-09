/// 测试类型: unit
/// 目标单元: include/aurora/widget/divider.h
/// 测试说明: 覆盖 Divider——默认属性、水平/垂直方向的布局尺寸（填满主轴、厚度作交叉轴）、
/// 厚度与紧约束钳制、缩进链式 setter、序列化往返与 describe 元数据

#include <string>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/divider.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_divider {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const Divider d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "Divider");

    Json props;
    d.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["orientation"].get<std::string>(), "horizontal");
    AURORA_TEST_CHECK_NEAR(props["thickness"].get<float>(), 1.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(props["color"][0].get<int>(), 200);
    AURORA_TEST_CHECK_EQ(props["color"][3].get<int>(), 255);
    AURORA_TEST_CHECK_NEAR(props["indent"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["end_indent"].get<float>(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(horizontal_fills_width_uses_thickness_height) {
    Divider d;
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 300.0F, 1e-4F);  // 横向填满父宽
    AURORA_TEST_CHECK_NEAR(d.size().height, 1.0F, 1e-4F);  // 高度 = 厚度
}

AURORA_TEST_CASE(vertical_fills_height_uses_thickness_width) {
    Divider d{DividerProps{.orientation = Orientation::Vertical}};
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 1.0F, 1e-4F);  // 宽度 = 厚度
    AURORA_TEST_CHECK_NEAR(d.size().height, 80.0F, 1e-4F);  // 纵向填满父高
}

AURORA_TEST_CASE(thickness_drives_cross_axis_size) {
    Divider h{DividerProps{.thickness = 4.0F}};
    LayoutEngine::layout(h, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(h.size().height, 4.0F, 1e-4F);

    Divider v{DividerProps{.orientation = Orientation::Vertical, .thickness = 3.0F}};
    LayoutEngine::layout(v, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(v.size().width, 3.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Divider d{DividerProps{.thickness = 6.0F}};
    LayoutEngine::layout(d, bounded(20.0F, 2.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.size().height, 2.0F, 1e-4F);  // 请求厚度 6 被钳到 max 2
}

AURORA_TEST_CASE(indent_setters_chain_and_serialize) {
    Divider d;
    d.set_indent(8.0F).set_end_indent(4.0F);  // 链式

    Json props;
    d.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["indent"].get<float>(), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["end_indent"].get<float>(), 4.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Divider src{DividerProps{.orientation = Orientation::Vertical,
                             .thickness = 2.5F,
                             .color = Color(10, 20, 30, 40),
                             .indent = 6.0F,
                             .end_indent = 3.0F}};
    Json props;
    src.serialize_props(props);

    Divider dst;
    dst.deserialize_props(props);
    Json out;
    dst.serialize_props(out);
    AURORA_TEST_CHECK_EQ(out["orientation"].get<std::string>(), "vertical");
    AURORA_TEST_CHECK_NEAR(out["thickness"].get<float>(), 2.5F, 1e-4F);
    AURORA_TEST_CHECK_EQ(out["color"][1].get<int>(), 20);
    AURORA_TEST_CHECK_EQ(out["color"][3].get<int>(), 40);
    AURORA_TEST_CHECK_NEAR(out["indent"].get<float>(), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out["end_indent"].get<float>(), 3.0F, 1e-4F);

    // 方向随 JSON 翻转：布局尺寸按垂直语义重算。
    LayoutEngine::layout(dst, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 2.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Divider::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Divider");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_orientation = false;
    bool has_thickness = false;
    bool has_indent = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "orientation") {
            has_orientation = true;
        }
        if (std::string{p.name} == "thickness") {
            has_thickness = true;
        }
        if (std::string{p.name} == "indent") {
            has_indent = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_orientation);
    AURORA_TEST_CHECK_TRUE(has_thickness);
    AURORA_TEST_CHECK_TRUE(has_indent);
}

}  // namespace aurora::test_cases::utest_divider
