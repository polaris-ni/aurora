/// 测试类型: unit
/// 目标单元: include/aurora/core/transform.h
/// 测试说明: 覆盖 Matrix2D 平移/旋转/缩放工厂、compose 求值顺序、绕点变换、逆矩阵往返与退化降级路径

#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"  // 退化路径会经 Diagnostics::degraded 上报，用 take() 消费
#include "aurora/core/transform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_transform {

namespace au = aurora;
namespace m = aurora::testing::matchers;

/// @brief 默认构造即单位矩阵（2x3 恒等），is_identity 为真。
AURORA_TEST_CASE(default_matrix_is_identity) {
    constexpr aurora::Matrix2D mat{};
    static_assert(mat.m11 == 1.0F && mat.m12 == 0.0F && mat.m21 == 0.0F && mat.m22 == 1.0F);
    static_assert(mat.tx == 0.0F && mat.ty == 0.0F);
    AURORA_TEST_CHECK_TRUE(mat.is_identity());
}

/// @brief 平移矩阵把点整体偏移 (tx, ty)。
AURORA_TEST_CASE(translate_maps_points) {
    const auto mat = aurora::Matrix2D::from_translate(3.0F, -2.0F);
    AURORA_TEST_CHECK_FALSE(mat.is_identity());
    const auto moved = mat.apply_to_point({.x = 1.0F, .y = 1.0F});
    AURORA_TEST_CHECK_NEAR(moved.x, 4.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(moved.y, -1.0F, 1e-6F);
    const auto origin = mat.apply_to_point({.x = 0.0F, .y = 0.0F});
    AURORA_TEST_CHECK_NEAR(origin.x, 3.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(origin.y, -2.0F, 1e-6F);
}

/// @brief 旋转按屏幕坐标（y 向下）顺时针为正：90° 使 (1,0) -> (0,1)；360° 近似回到单位阵。
AURORA_TEST_CASE(rotate_turns_screen_clockwise) {
    const auto identity = aurora::Matrix2D::from_rotate(0.0F);
    AURORA_TEST_CHECK_TRUE(identity.is_identity());

    const auto quarter = aurora::Matrix2D::from_rotate(90.0F);
    const auto turned_x = quarter.apply_to_point({.x = 1.0F, .y = 0.0F});
    AURORA_TEST_CHECK_NEAR(turned_x.x, 0.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(turned_x.y, 1.0F, 1e-5F);
    const auto turned_y = quarter.apply_to_point({.x = 0.0F, .y = 1.0F});
    AURORA_TEST_CHECK_NEAR(turned_y.x, -1.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(turned_y.y, 0.0F, 1e-5F);

    const auto full = aurora::Matrix2D::from_rotate(360.0F);
    const auto back = full.apply_to_point({.x = 1.0F, .y = 0.0F});
    AURORA_TEST_CHECK_NEAR(back.x, 1.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(back.y, 0.0F, 1e-5F);
}

/// @brief 非均匀缩放对两轴独立作用。
AURORA_TEST_CASE(scale_maps_axes_independently) {
    const auto mat = aurora::Matrix2D::from_scale(2.0F, 3.0F);
    const auto scaled = mat.apply_to_point({.x = 2.0F, .y = 5.0F});
    AURORA_TEST_CHECK_NEAR(scaled.x, 4.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(scaled.y, 15.0F, 1e-6F);
    const auto unit = mat.apply_to_point({.x = 1.0F, .y = 1.0F});
    AURORA_TEST_CHECK_NEAR(unit.x, 2.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(unit.y, 3.0F, 1e-6F);
}

/// @brief compose 契约：a.compose(b) 先应用 b、再应用 a（this * o）。
AURORA_TEST_CASE(compose_applies_right_operand_first) {
    const auto translate = aurora::Matrix2D::from_translate(10.0F, 0.0F);
    const auto scale = aurora::Matrix2D::from_scale(2.0F, 2.0F);

    // 平移 ∘ 缩放：先缩放 (1,1)->(2,2)，再平移 -> (12,2)。
    const auto ts = translate.compose(scale);
    const auto via_scale_first = ts.apply_to_point({.x = 1.0F, .y = 1.0F});
    AURORA_TEST_CHECK_NEAR(via_scale_first.x, 12.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(via_scale_first.y, 2.0F, 1e-6F);

    // 反序合成结果不同：先平移 (1,1)->(11,1)，再缩放 -> (22,2)。
    const auto st = scale.compose(translate);
    const auto via_translate_first = st.apply_to_point({.x = 1.0F, .y = 1.0F});
    AURORA_TEST_CHECK_NEAR(via_translate_first.x, 22.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(via_translate_first.y, 2.0F, 1e-6F);
}

/// @brief from_rotate_about：中心点不动，偏移点按旋转矩阵绕中心转动。
AURORA_TEST_CASE(rotate_about_keeps_center_fixed) {
    constexpr aurora::Point center{.x = 5.0F, .y = 7.0F};
    const auto mat = aurora::Matrix2D::from_rotate_about(90.0F, center);

    const auto pinned = mat.apply_to_point(center);
    AURORA_TEST_CHECK_NEAR(pinned.x, center.x, 1e-5F);
    AURORA_TEST_CHECK_NEAR(pinned.y, center.y, 1e-5F);

    // 中心右侧 1px 的点，顺时针 90° 后转到中心下方 1px。
    const auto rotated = mat.apply_to_point({.x = 6.0F, .y = 7.0F});
    AURORA_TEST_CHECK_NEAR(rotated.x, 5.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(rotated.y, 8.0F, 1e-5F);
}

/// @brief from_scale_about：中心点不动，偏移按非均匀缩放展开。
AURORA_TEST_CASE(scale_about_keeps_center_fixed) {
    constexpr aurora::Point center{.x = 4.0F, .y = 6.0F};
    const auto mat = aurora::Matrix2D::from_scale_about(2.0F, 3.0F, center);

    const auto pinned = mat.apply_to_point(center);
    AURORA_TEST_CHECK_NEAR(pinned.x, center.x, 1e-6F);
    AURORA_TEST_CHECK_NEAR(pinned.y, center.y, 1e-6F);

    const auto expanded = mat.apply_to_point({.x = 5.0F, .y = 7.0F});  // 中心 + (1,1)
    AURORA_TEST_CHECK_NEAR(expanded.x, 6.0F, 1e-6F);  // 中心 + (2,·)
    AURORA_TEST_CHECK_NEAR(expanded.y, 9.0F, 1e-6F);  // 中心 + (·,3)
}

/// @brief 可逆矩阵的 inverse 满足 M ∘ M⁻¹ ≈ 恒等（往返误差远小于 1 像素）。
AURORA_TEST_CASE(inverse_roundtrips_for_invertible_matrix) {
    const auto mat = aurora::Matrix2D::from_translate(5.0F, 7.0F).compose(aurora::Matrix2D::from_rotate(30.0F));
    const auto inv = mat.inverse();

    constexpr aurora::Point p{.x = 3.0F, .y = 4.0F};
    const auto forward_then_back = inv.apply_to_point(mat.apply_to_point(p));
    AURORA_TEST_CHECK_NEAR(forward_then_back.x, p.x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(forward_then_back.y, p.y, 1e-4F);

    const auto back_then_forward = mat.apply_to_point(inv.apply_to_point(p));
    AURORA_TEST_CHECK_NEAR(back_then_forward.x, p.x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(back_then_forward.y, p.y, 1e-4F);
}

/// @brief 退化矩阵（行列式≈0）求逆降级为单位矩阵，并上报 matrix2d-degenerate 诊断（需求 #21）。
/// @note 默认 strict mode 为 Off，degraded 只记录不终止；take() 是测试消费诊断的约定接口，
///       且 runner 为文件级进程隔离，无跨用例污染。
AURORA_TEST_CASE(degenerate_inverse_degrades_to_identity) {
    const auto degenerate = aurora::Matrix2D::from_scale(0.0F, 0.0F);
    const auto restored = degenerate.inverse();
    AURORA_TEST_CHECK_TRUE(restored.is_identity());

    std::vector<std::string> codes;
    for (const auto& diagnostic : aurora::Diagnostics::take()) {
        if (!diagnostic.code.empty()) {
            codes.push_back(diagnostic.code);
        }
    }
    AURORA_TEST_CHECK_THAT(codes, m::contains(std::string{"matrix2d-degenerate"}));
}

}  // namespace aurora::test_cases::utest_transform
