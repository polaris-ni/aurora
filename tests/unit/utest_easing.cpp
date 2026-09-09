/// 测试类型: unit
/// 目标单元: include/aurora/animation/easing.h
/// 测试说明: 覆盖 Curve 默认/命名/自定义构造、transform 的输入输出夹取、命名曲线端点与闭式公式、in_out 对称性与单调性、BounceOut 落点及无函数自定义曲线的线性回退

#include <utility>
#include <vector>

#include "aurora/animation/easing.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_easing {

namespace m = aurora::testing::matchers;

/// @brief 默认构造即 Linear 曲线：transform 为恒等映射。
AURORA_TEST_CASE(default_curve_is_linear) {
    const aurora::Curve c;
    AURORA_TEST_CHECK_TRUE(c.kind() == aurora::CurveKind::Linear);
    AURORA_TEST_CHECK_NEAR(c.transform(0.0), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(c.transform(0.25), 0.25, 1e-12);
    AURORA_TEST_CHECK_NEAR(c.transform(0.5), 0.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(c.transform(1.0), 1.0, 1e-12);
}

/// @brief transform 把输入夹入 [0,1]，并把（自定义曲线的）输出也夹入 [0,1]。
AURORA_TEST_CASE(transform_clamps_input_and_output) {
    const aurora::Curve linear;
    AURORA_TEST_CHECK_NEAR(linear.transform(-0.5), 0.0, 1e-12);  // 输入下界夹取
    AURORA_TEST_CHECK_NEAR(linear.transform(1.7), 1.0, 1e-12);   // 输入上界夹取

    // 自定义曲线输出越界同样被夹取。
    const aurora::Curve overshoot{[](double) { return 2.0; }};
    AURORA_TEST_CHECK_NEAR(overshoot.transform(0.5), 1.0, 1e-12);
    const aurora::Curve negative{[](double) { return -1.0; }};
    AURORA_TEST_CHECK_NEAR(negative.transform(0.5), 0.0, 1e-12);

    // 全输入域采样：输出始终落在 [0,1]。
    std::vector<double> results;
    for (int i = -5; i <= 15; ++i) {
        results.push_back(linear.transform(static_cast<double>(i) / 10.0));
    }
    AURORA_TEST_CHECK_THAT(results, m::each(m::all_of(m::ge(0.0), m::le(1.0))));
}

/// @brief 全部命名曲线工厂的曲线在端点处精确命中 0 与 1。
AURORA_TEST_CASE(named_curves_hit_exact_endpoints) {
    const std::vector<std::pair<const char*, aurora::Curve>> all = {
        {"linear", aurora::Curves::linear()},
        {"ease_in", aurora::Curves::ease_in()},
        {"ease_out", aurora::Curves::ease_out()},
        {"ease_in_out", aurora::Curves::ease_in_out()},
        {"ease_in_sine", aurora::Curves::ease_in_sine()},
        {"ease_out_sine", aurora::Curves::ease_out_sine()},
        {"ease_in_out_sine", aurora::Curves::ease_in_out_sine()},
        {"ease_in_quad", aurora::Curves::ease_in_quad()},
        {"ease_out_quad", aurora::Curves::ease_out_quad()},
        {"ease_in_out_quad", aurora::Curves::ease_in_out_quad()},
        {"ease_in_cubic", aurora::Curves::ease_in_cubic()},
        {"ease_out_cubic", aurora::Curves::ease_out_cubic()},
        {"ease_in_out_cubic", aurora::Curves::ease_in_out_cubic()},
        {"bounce_out", aurora::Curves::bounce_out()},
    };
    for (const auto& entry : all) {
        AURORA_TEST_TRACE(entry.first);
        AURORA_TEST_CHECK_NEAR(entry.second.transform(0.0), 0.0, 1e-9);
        AURORA_TEST_CHECK_NEAR(entry.second.transform(1.0), 1.0, 1e-9);
    }
}

/// @brief 多项式/正弦命名曲线在中点处的值与闭式公式一致，工厂 kind 标注正确。
AURORA_TEST_CASE(polynomial_curves_match_closed_form) {
    AURORA_TEST_CHECK_TRUE(aurora::Curves::linear().kind() == aurora::CurveKind::Linear);
    AURORA_TEST_CHECK_TRUE(aurora::Curves::ease_in().kind() == aurora::CurveKind::EaseIn);
    AURORA_TEST_CHECK_TRUE(aurora::Curves::bounce_out().kind() == aurora::CurveKind::BounceOut);

    const aurora::Curve in_quad = aurora::Curves::ease_in_quad();
    AURORA_TEST_CHECK_NEAR(in_quad.transform(0.5), 0.25, 1e-12);  // t²
    const aurora::Curve out_quad = aurora::Curves::ease_out_quad();
    AURORA_TEST_CHECK_NEAR(out_quad.transform(0.5), 0.75, 1e-12);  // t(2-t)
    const aurora::Curve in_out_quad = aurora::Curves::ease_in_out_quad();
    AURORA_TEST_CHECK_NEAR(in_out_quad.transform(0.25), 0.125, 1e-12);
    AURORA_TEST_CHECK_NEAR(in_out_quad.transform(0.75), 0.875, 1e-12);

    const aurora::Curve in_cubic = aurora::Curves::ease_in_cubic();
    AURORA_TEST_CHECK_NEAR(in_cubic.transform(0.5), 0.125, 1e-12);  // t³
    const aurora::Curve out_cubic = aurora::Curves::ease_out_cubic();
    AURORA_TEST_CHECK_NEAR(out_cubic.transform(0.5), 0.875, 1e-12);  // 1-(1-t)³
    const aurora::Curve in_out_cubic = aurora::Curves::ease_in_out_cubic();
    AURORA_TEST_CHECK_NEAR(in_out_cubic.transform(0.25), 0.0625, 1e-12);
    const aurora::Curve in_out = aurora::Curves::ease_in_out();
    AURORA_TEST_CHECK_NEAR(in_out.transform(0.25), 0.0625, 1e-12);  // 与 cubic 同式

    constexpr double sqrt_half = 0.7071067811865476;  // √2/2
    const aurora::Curve in_sine = aurora::Curves::ease_in_sine();
    AURORA_TEST_CHECK_NEAR(in_sine.transform(0.5), 1.0 - sqrt_half, 1e-9);
    const aurora::Curve out_sine = aurora::Curves::ease_out_sine();
    AURORA_TEST_CHECK_NEAR(out_sine.transform(0.5), sqrt_half, 1e-9);
    const aurora::Curve in_out_sine = aurora::Curves::ease_in_out_sine();
    AURORA_TEST_CHECK_NEAR(in_out_sine.transform(0.5), 0.5, 1e-9);
}

/// @brief in_out 家族关于中点对称（f(t)+f(1-t)=1）、中点值 0.5 且采样单调不减。
AURORA_TEST_CASE(in_out_curves_are_symmetric_and_monotonic) {
    const std::vector<std::pair<const char*, aurora::Curve>> in_out_family = {
        {"ease_in_out", aurora::Curves::ease_in_out()},
        {"ease_in_out_sine", aurora::Curves::ease_in_out_sine()},
        {"ease_in_out_quad", aurora::Curves::ease_in_out_quad()},
        {"ease_in_out_cubic", aurora::Curves::ease_in_out_cubic()},
    };
    for (const auto& entry : in_out_family) {
        AURORA_TEST_TRACE(entry.first);
        AURORA_TEST_CHECK_NEAR(entry.second.transform(0.5), 0.5, 1e-9);
        for (int i = 0; i <= 5; ++i) {
            const double t = static_cast<double>(i) / 10.0;
            AURORA_TEST_CHECK_NEAR(entry.second.transform(t) + entry.second.transform(1.0 - t), 1.0, 1e-9);
        }
        double prev = 0.0;
        for (int i = 0; i <= 10; ++i) {
            const double v = entry.second.transform(static_cast<double>(i) / 10.0);
            AURORA_TEST_CHECK_GE(v, prev);
            prev = v;
        }
    }
}

/// @brief BounceOut 在各段反弹谷底/边界处的值与经典分段公式一致（非单调）。
AURORA_TEST_CASE(bounce_out_hits_known_bounce_valleys) {
    const aurora::Curve bounce = aurora::Curves::bounce_out();
    AURORA_TEST_CHECK_NEAR(bounce.transform(0.0), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(bounce.transform(0.5), 0.765625, 1e-12);  // 第一段回落谷
    AURORA_TEST_CHECK_NEAR(bounce.transform(1.0 / 2.75), 1.0, 1e-12);  // 第一段触顶
    AURORA_TEST_CHECK_NEAR(bounce.transform(1.5 / 2.75), 0.75, 1e-12);  // 第二段谷底
    AURORA_TEST_CHECK_NEAR(bounce.transform(2.0 / 2.75), 1.0, 1e-12);  // 第二段触顶
    AURORA_TEST_CHECK_NEAR(bounce.transform(2.25 / 2.75), 0.9375, 1e-12);  // 第三段谷底
    AURORA_TEST_CHECK_NEAR(bounce.transform(2.625 / 2.75), 0.984375, 1e-12);  // 第四段谷底
    AURORA_TEST_CHECK_NEAR(bounce.transform(1.0), 1.0, 1e-12);
}

/// @brief 自定义曲线按函数求值且 kind 为 Custom；指定 Custom 却无函数时回退线性。
AURORA_TEST_CASE(custom_curve_applies_function_with_linear_fallback) {
    const aurora::Curve stepped{[](double t) { return t < 0.5 ? 0.0 : 1.0; }};
    AURORA_TEST_CHECK_TRUE(stepped.kind() == aurora::CurveKind::Custom);
    AURORA_TEST_CHECK_NEAR(stepped.transform(0.25), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(stepped.transform(0.75), 1.0, 1e-12);

    // 只有 kind、没有 std::function 的自定义曲线：eval 走 Custom 分支返回 t（线性回退）。
    const aurora::Curve bare{aurora::CurveKind::Custom};
    AURORA_TEST_CHECK_TRUE(bare.kind() == aurora::CurveKind::Custom);
    AURORA_TEST_CHECK_NEAR(bare.transform(0.25), 0.25, 1e-12);
    AURORA_TEST_CHECK_NEAR(bare.transform(0.9), 0.9, 1e-12);
}

}  // namespace aurora::test_cases::utest_easing
