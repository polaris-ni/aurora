/// 测试类型: unit
/// 目标单元: include/aurora/widget/containers.h
/// 测试说明: 覆盖 Column/Row 容器级行为——初始化列表构造与所有权、gap 落位、
/// MainAxisSize::Max 撑满、MainAxisAlignment::End 收尾对齐、CrossAxisAlignment::Stretch
/// 拉伸子项交叉轴、负 gap 校验、属性序列化往返

#include <memory>
#include <string>

#include "aurora/core/directionality.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_containers {

namespace {

/// 固定尺寸文本盒（宽高锁定，尺寸可预期）。
auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

/// 交叉轴可拉伸盒（对应轴用 Expand）。
auto box_cross_expand(float main_extent, bool stretch_w) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(stretch_w ? aurora::Length::expand() : aurora::Length::fixed(main_extent));
    t->height(stretch_w ? aurora::Length::fixed(main_extent) : aurora::Length::expand());
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(initializer_list_registers_children_in_order) {
    Column col{box(10.0F, 10.0F), box(20.0F, 10.0F), box(30.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(std::string{col.type_name()}, "Column");
    AURORA_TEST_REQUIRE_EQ(col.child_nodes().size(), 3U);
    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().size.width, 10.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().size.width, 20.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[2].bounds().size.width, 30.0F, 0.0F);
    // 纵向依序落位：第 2 项 y = 10 + 10（前项高 + 无 gap）。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[2].bounds().origin.y, 20.0F, 1e-4F);

    Row row{box(10.0F, 10.0F), box(20.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(std::string{row.type_name()}, "Row");
    AURORA_TEST_REQUIRE_EQ(row.child_nodes().size(), 2U);
    LayoutEngine::layout(row, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 10.0F, 1e-4F);
}

AURORA_TEST_CASE(column_gap_places_children_vertically) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.add(box(100.0F, 20.0F));
    col.set_gap(10.0F);

    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.y, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(row_gap_places_children_horizontally) {
    Row row;
    row.add(box(40.0F, 20.0F));
    row.add(box(60.0F, 20.0F));
    row.set_gap(8.0F);

    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(column_main_axis_size_max_fills_height) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.set_main_axis_size(MainAxisSize::Max);

    LayoutEngine::layout(col, bounded(200.0F, 150.0F));
    const Size s = col.size();
    AURORA_TEST_CHECK_NEAR(s.height, 150.0F, 1e-4F);
    // 默认 Start 对齐：子项仍在顶部。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(column_main_axis_end_pushes_children_to_bottom) {
    Column col;
    col.add(box(100.0F, 20.0F));
    col.add(box(100.0F, 20.0F));
    col.set_main_axis_size(MainAxisSize::Max);
    col.set_main_axis_alignment(MainAxisAlignment::End);

    LayoutEngine::layout(col, bounded(200.0F, 200.0F));
    // 占用 40，End：首项 y = 200-40 = 160。
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().origin.y, 160.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[1].bounds().origin.y, 180.0F, 1e-4F);
}

AURORA_TEST_CASE(cross_axis_stretch_expands_child) {
    // Row 交叉轴（竖直）Stretch：height 为 Expand 的子项被拉伸到容器高。
    Row row;
    row.add(box_cross_expand(30.0F, false));  // 宽固定 30，高 Expand
    row.set_cross_axis_alignment(CrossAxisAlignment::Stretch);

    LayoutEngine::layout(row, bounded(300.0F, 100.0F));
    const Size s = row.size();
    AURORA_TEST_CHECK_NEAR(s.height, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().size.height, 100.0F, 1e-4F);

    // Column 交叉轴（水平）Stretch：width 为 Expand 的子项被拉伸到容器宽。
    Column col;
    col.add(box_cross_expand(20.0F, true));  // 宽 Expand，高固定 20
    col.set_cross_axis_alignment(CrossAxisAlignment::Stretch);
    LayoutEngine::layout(col, bounded(200.0F, 100.0F));
    const Size cs = col.size();
    AURORA_TEST_CHECK_NEAR(cs.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(col.child_nodes()[0].bounds().size.width, 200.0F, 1e-4F);
}

AURORA_TEST_CASE(validate_props_rejects_negative_gap) {
    Column bad;
    bad.set_gap(-1.0F);
    AURORA_TEST_CHECK_FALSE(bad.validate_props().ok());

    Column good;
    good.set_gap(4.0F);
    AURORA_TEST_CHECK_TRUE(good.validate_props().ok());
}

AURORA_TEST_CASE(props_serialize_deserialize_roundtrip) {
    Column src;
    src.set_main_axis_alignment(MainAxisAlignment::SpaceBetween);
    src.set_cross_axis_alignment(CrossAxisAlignment::Center);
    src.set_main_axis_size(MainAxisSize::Max);
    src.set_gap(12.0F);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["main_axis_alignment"].get<std::string>(), "SpaceBetween");
    AURORA_TEST_CHECK_EQ(props["gap"].get<float>(), 12.0F);

    Column dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.flex.main_axis == MainAxisAlignment::SpaceBetween);
    AURORA_TEST_CHECK_TRUE(dst.flex.cross_axis == CrossAxisAlignment::Center);
    AURORA_TEST_CHECK_TRUE(dst.flex.main_axis_size == MainAxisSize::Max);
    AURORA_TEST_CHECK_NEAR(dst.gap, 12.0F, 0.0F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Column::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Column");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    bool has_gap = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "gap") {
            has_gap = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_gap);
}

AURORA_TEST_CASE(row_rtl_via_environment_mirrors_children) {
    // A2 布局镜像端到端：经 Environment 注入 Directionality(RTL) 后，
    // Row 子项视觉顺序翻转（Start 排布首项贴右缘）——容器按 resolved_text_direction 注入。
    const Environment env =
        Environment{}.with<Directionality>(Directionality{.direction = TextDirection::RTL, .host_set = true});
    BuildContext ctx;
    ctx.env = &env;

    Row row;
    row.add(box(40.0F, 20.0F));
    row.add(box(60.0F, 20.0F));

    row.mount(ctx);
    row.layout(bounded(300.0F, 100.0F), ctx);

    // 内容宽 100：LTR 时 child0@0、child1@40；RTL 镜像后 child0@60、child1@0。
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[0].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(row.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);

    // 无环境注入的对照组：保持 LTR 物理序（镜像不生效）。
    Row ltr;
    ltr.add(box(40.0F, 20.0F));
    ltr.add(box(60.0F, 20.0F));
    BuildContext plain;
    ltr.mount(plain);
    ltr.layout(bounded(300.0F, 100.0F), plain);
    AURORA_TEST_CHECK_NEAR(ltr.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(ltr.child_nodes()[1].bounds().origin.x, 40.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_containers
