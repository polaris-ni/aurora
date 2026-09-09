/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_transform.h
/// 测试说明: 覆盖 Transform 切片三节点——AlignNode 占满/无限约束退化与 child_size 记录、
/// OffsetNode 视觉偏移不改布局、TransformNode 旋转/缩放/原始矩阵绕内容中心构造与布局透传

#include "aurora/modifier/modifier_transform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier_transform {

namespace {

auto make_measure(float w, float h) -> std::function<Size(const Constraints&)> {
    return [w, h](const Constraints&) -> Size { return Size{.width = w, .height = h}; };
}

auto constraints(float max_w, float max_h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = max_w, .height = max_h}};
}

}  // namespace

AURORA_TEST_CASE(align_fills_available_space_and_records_child_size) {
    // Align 默认占满父约束；child_size 记录子测量结果（绘制期据此平移内容）。
    const AlignNode a(Alignment::Center);
    AURORA_TEST_CHECK_EQ(a.kind(), ModifierNode::Kind::Transform);
    const Size s = a.layout(constraints(200.0F, 100.0F), make_measure(50.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(s.width, 200.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 100.0F, 0.0F);
    const Size child = a.child_size();
    AURORA_TEST_CHECK_NEAR(child.width, 50.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(child.height, 20.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(a.align(), Alignment::Center);
}

AURORA_TEST_CASE(align_degrades_to_child_size_under_infinite_constraint) {
    // 父约束无限（max=∞）时退化为内容尺寸。
    const AlignNode a(Alignment::TopLeft);
    const Size s = a.layout(Constraints{}, make_measure(30.0F, 40.0F));
    AURORA_TEST_CHECK_NEAR(s.width, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 40.0F, 0.0F);
}

AURORA_TEST_CASE(align_respects_min_constraint_clamp) {
    // max 无限时 self 取子尺寸，结果经 c.constrain 被 min 抬升。
    const AlignNode a(Alignment::TopLeft);
    Constraints c{.min = Size{.width = 60.0F, .height = 60.0F}, .max = Size::infinity()};
    const Size s = a.layout(c, make_measure(10.0F, 10.0F));
    AURORA_TEST_CHECK_NEAR(s.width, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 60.0F, 0.0F);
}

AURORA_TEST_CASE(offset_translates_visually_without_layout_change) {
    const OffsetNode o(3.0F, -7.0F);
    AURORA_TEST_CHECK_EQ(o.kind(), ModifierNode::Kind::Transform);
    AURORA_TEST_CHECK_NEAR(o.dx(), 3.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(o.dy(), -7.0F, 0.0F);
    // 布局尺寸=子尺寸（仅约束夹取），偏移不影响布局。
    const Size s = o.layout(constraints(100.0F, 100.0F), make_measure(80.0F, 60.0F));
    AURORA_TEST_CHECK_NEAR(s.width, 80.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 60.0F, 0.0F);
}

AURORA_TEST_CASE(transform_node_rotate_builds_matrix_about_center) {
    // 绕 (100,50) 旋转 90°：(1,0)->应落在中心右侧偏移处。锁定矩阵参数即可。
    const TransformNode t(90.0F);
    AURORA_TEST_CHECK_EQ(t.kind(), ModifierNode::Kind::Transform);
    const Matrix2D m = t.matrix(Size{.width = 200.0F, .height = 100.0F});
    // 与 from_rotate_about 参考实现逐项一致。
    const Matrix2D ref = Matrix2D::from_rotate_about(90.0F, Point{.x = 100.0F, .y = 50.0F});
    AURORA_TEST_CHECK_NEAR(m.m11, ref.m11, 1e-5F);
    AURORA_TEST_CHECK_NEAR(m.m12, ref.m12, 1e-5F);
    AURORA_TEST_CHECK_NEAR(m.m21, ref.m21, 1e-5F);
    AURORA_TEST_CHECK_NEAR(m.m22, ref.m22, 1e-5F);
    AURORA_TEST_CHECK_NEAR(m.tx, ref.tx, 1e-4F);
    AURORA_TEST_CHECK_NEAR(m.ty, ref.ty, 1e-4F);
    // 90° 旋转把 (cx+r, cy) 映到 (cx, cy+r)：绕中心旋转的方向语义。
    const Point p = m.apply_to_point(Point{.x = 150.0F, .y = 50.0F});
    AURORA_TEST_CHECK_NEAR(p.x, 100.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(p.y, 100.0F, 1e-3F);
}

AURORA_TEST_CASE(transform_node_scale_matrix_about_center) {
    const TransformNode t(2.0F, 3.0F);
    const Matrix2D m = t.matrix(Size{.width = 100.0F, .height = 60.0F});
    // 绕中心 (50,30) 缩放：(50,30) 不动，(60,30)->(70,30)。
    const Point fixed = m.apply_to_point(Point{.x = 50.0F, .y = 30.0F});
    AURORA_TEST_CHECK_NEAR(fixed.x, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fixed.y, 30.0F, 1e-4F);
    const Point scaled = m.apply_to_point(Point{.x = 60.0F, .y = 30.0F});
    AURORA_TEST_CHECK_NEAR(scaled.x, 70.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(scaled.y, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(transform_node_raw_matrix_passthrough) {
    const Matrix2D raw = Matrix2D::from_translate(5.0F, 6.0F);
    const TransformNode t(raw);
    const Matrix2D m = t.matrix(Size{.width = 10.0F, .height = 10.0F});
    // Raw 原样返回用户矩阵（中心化责任在调用方）。
    AURORA_TEST_CHECK_NEAR(m.tx, 5.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(m.ty, 6.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(m.m11, 1.0F, 0.0F);
}

AURORA_TEST_CASE(transform_node_layout_is_passthrough) {
    // 旋转/缩放不改布局尺寸。
    const TransformNode rot(45.0F);
    const Size s1 = rot.layout(constraints(100.0F, 100.0F), make_measure(70.0F, 30.0F));
    AURORA_TEST_CHECK_NEAR(s1.width, 70.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s1.height, 30.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_modifier_transform
