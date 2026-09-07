/// 测试类型: unit
/// 目标单元: include/aurora/core/transform.h
/// 测试说明: 2D 仿射矩阵 Matrix2D（平移/旋转/缩放/合成/求逆/退化降级/点映射）单元测试

#include <cmath>

#include "aurora/core/diagnostics.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_transform {

namespace {

[[nodiscard]] auto near(float a, float b) -> bool { return std::fabs(a - b) < 1e-4F; }

[[nodiscard]] auto near_point(Point a, Point b) -> bool { return near(a.x, b.x) && near(a.y, b.y); }

}  // namespace

AURORA_TEST() {
    // ---- 1. 默认矩阵即单位矩阵 ----
    {
        const Matrix2D m{};
        AURORA_TEST_CHECK(m.is_identity());
        AURORA_TEST_CHECK(near_point(m.apply_to_point(Point{.x = 3.0F, .y = 7.0F}), Point{.x = 3.0F, .y = 7.0F}));
    }

    // ---- 2. 平移矩阵：仅位移，不改变相对结构 ----
    {
        const auto m = Matrix2D::from_translate(10.0F, -4.0F);
        const auto p = m.apply_to_point(Point{.x = 1.0F, .y = 2.0F});
        AURORA_TEST_CHECK(near_point(p, Point{.x = 11.0F, .y = -2.0F}));
        AURORA_TEST_CHECK(!m.is_identity());
    }

    // ---- 3. 旋转 90°：(1,0) -> (0,1)（屏幕 y 轴向下，顺时针为正） ----
    {
        const auto m = Matrix2D::from_rotate(90.0F);
        const auto p = m.apply_to_point(Point{.x = 1.0F, .y = 0.0F});
        AURORA_TEST_CHECK(near_point(p, Point{.x = 0.0F, .y = 1.0F}));
    }

    // ---- 4. 旋转 360° 回到原点附近（单位矩阵等价） ----
    {
        const auto m = Matrix2D::from_rotate(360.0F);
        const auto p = m.apply_to_point(Point{.x = 5.0F, .y = 0.0F});
        AURORA_TEST_CHECK(near_point(p, Point{.x = 5.0F, .y = 0.0F}));
    }

    // ---- 5. 非均匀缩放：x / y 独立 ----
    {
        const auto m = Matrix2D::from_scale(2.0F, 3.0F);
        const auto p = m.apply_to_point(Point{.x = 4.0F, .y = 1.0F});
        AURORA_TEST_CHECK(near_point(p, Point{.x = 8.0F, .y = 3.0F}));
    }

    // ---- 6. compose 语义：this * o，先应用 o 再应用 this ----
    {
        const auto t = Matrix2D::from_translate(10.0F, 0.0F);
        const auto r = Matrix2D::from_rotate(90.0F);
        const auto tr = t.compose(r);  // 先旋转再平移
        AURORA_TEST_CHECK(near_point(tr.apply_to_point(Point{.x = 1.0F, .y = 0.0F}),
                                     Point{.x = 10.0F, .y = 1.0F}));

        const auto rt = r.compose(t);  // 先平移再旋转
        AURORA_TEST_CHECK(near_point(rt.apply_to_point(Point{.x = 1.0F, .y = 0.0F}),
                                     Point{.x = 0.0F, .y = 11.0F}));
    }

    // ---- 7. 求逆：M 与其逆复合回到单位矩阵 ----
    {
        const auto m = Matrix2D::from_scale(2.0F, 3.0F).compose(Matrix2D::from_translate(5.0F, 7.0F));
        const auto id = m.compose(m.inverse());
        AURORA_TEST_CHECK(near(id.m11, 1.0F));
        AURORA_TEST_CHECK(near(id.m22, 1.0F));
        AURORA_TEST_CHECK(near(id.m12, 0.0F));
        AURORA_TEST_CHECK(near(id.m21, 0.0F));
        AURORA_TEST_CHECK(near(id.tx, 0.0F));
        AURORA_TEST_CHECK(near(id.ty, 0.0F));
    }

    // ---- 8. 逆变换把点映射回原处 ----
    {
        const auto m = Matrix2D::from_translate(12.0F, -5.0F);
        const Point src{.x = 2.0F, .y = 3.0F};
        const auto moved = m.apply_to_point(src);
        const auto back = m.inverse().apply_to_point(moved);
        AURORA_TEST_CHECK(near_point(back, src));
    }

    // ---- 9. 退化矩阵（缩放为 0）求逆降级为单位矩阵并上报诊断 ----
    {
        const auto before = Diagnostics::count();
        const auto inv = Matrix2D::from_scale(0.0F, 0.0F).inverse();
        AURORA_TEST_CHECK(inv.is_identity());
        AURORA_TEST_CHECK_GT(Diagnostics::count(), before);
    }

    // ---- 10. 绕任意点旋转 180°：中心对称 ----
    {
        const auto m = Matrix2D::from_rotate_about(180.0F, Point{.x = 10.0F, .y = 10.0F});
        const auto p = m.apply_to_point(Point{.x = 20.0F, .y = 10.0F});
        AURORA_TEST_CHECK(near_point(p, Point{.x = 0.0F, .y = 10.0F}));

        // 中心点自身不动
        AURORA_TEST_CHECK(near_point(m.apply_to_point(Point{.x = 10.0F, .y = 10.0F}),
                                     Point{.x = 10.0F, .y = 10.0F}));
    }

    // ---- 11. 绕任意点缩放：中心不动，其余按比例远离 ----
    {
        const auto m = Matrix2D::from_scale_about(2.0F, 2.0F, Point{.x = 10.0F, .y = 10.0F});
        AURORA_TEST_CHECK(near_point(m.apply_to_point(Point{.x = 10.0F, .y = 10.0F}),
                                     Point{.x = 10.0F, .y = 10.0F}));
        AURORA_TEST_CHECK(near_point(m.apply_to_point(Point{.x = 20.0F, .y = 10.0F}),
                                     Point{.x = 30.0F, .y = 10.0F}));
    }

    // ---- 12. is_identity 对微小旋转判定为非单位 ----
    {
        AURORA_TEST_CHECK(!Matrix2D::from_rotate(1.0F).is_identity());
        AURORA_TEST_CHECK(Matrix2D::from_scale(1.0F, 1.0F).is_identity());
        AURORA_TEST_CHECK(Matrix2D::from_translate(0.0F, 0.0F).is_identity());
    }
}

}  // namespace aurora::test_cases::utest_transform
