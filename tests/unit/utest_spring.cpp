/// 测试类型: unit
/// 目标单元: include/aurora/animation/spring.h
/// 测试说明: 覆盖 SpringDescription 派生量、SpringSimulation 三种阻尼 regimes
/// 的端点/收敛、临界阻尼无过冲、欠阻尼过冲回落、初速度塑形、数值微分速度与 is_settled 的位置+速度双条件

#include <numbers>

#include "aurora/animation/spring.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_spring {

/// @brief 默认参数与自定义参数下的固有频率、阻尼比与公式一致（欠/临界/过阻尼取整值）。
AURORA_TEST_CASE(spring_description_defaults_and_derived_quantities) {
    const aurora::SpringDescription defaults{};
    AURORA_TEST_CHECK_NEAR(defaults.natural_frequency(), 13.038404810405298, 1e-9);  // sqrt(170)
    // 默认参数接近临界（ζ ≈ 0.997）但不越过。
    AURORA_TEST_CHECK_GT(defaults.damping_ratio(), 0.99);
    AURORA_TEST_CHECK_LT(defaults.damping_ratio(), 1.0);

    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
    AURORA_TEST_CHECK_NEAR(under.natural_frequency(), 10.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(under.damping_ratio(), 0.1, 1e-12);

    const aurora::SpringDescription crit{.stiffness = 100.0, .damping = 20.0, .mass = 1.0};
    AURORA_TEST_CHECK_NEAR(crit.damping_ratio(), 1.0, 1e-12);

    const aurora::SpringDescription over{.stiffness = 100.0, .damping = 40.0, .mass = 1.0};
    AURORA_TEST_CHECK_NEAR(over.damping_ratio(), 2.0, 1e-12);
}

/// @brief 三种阻尼 regimes 下：t≤0 返回起点，长时间后收敛到终点。
AURORA_TEST_CASE(simulation_matches_endpoints_across_damping_regimes) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
    const aurora::SpringDescription crit{.stiffness = 100.0, .damping = 20.0, .mass = 1.0};
    const aurora::SpringDescription over{.stiffness = 100.0, .damping = 40.0, .mass = 1.0};
    const aurora::SpringSimulation under_sim{under, 0.0, 1.0};
    const aurora::SpringSimulation crit_sim{crit, 0.0, 1.0};
    const aurora::SpringSimulation over_sim{over, 0.0, 1.0};

    AURORA_TEST_CHECK_EQ(under_sim.target(), 1.0);
    const aurora::SpringSimulation* regimes[] = {&under_sim, &crit_sim, &over_sim};
    for (const aurora::SpringSimulation* sim : regimes) {
        AURORA_TEST_TRACE("regime");
        AURORA_TEST_CHECK_NEAR(sim->value(0.0), 0.0, 1e-12);  // t=0 在起点
        AURORA_TEST_CHECK_NEAR(sim->value(-0.5), 0.0, 1e-12);  // 负时间同样返回起点
        AURORA_TEST_CHECK_NEAR(sim->value(10.0), 1.0, 1e-4);  // 长时间收敛到终点
    }
}

/// @brief 临界阻尼从起点到终点无过冲、单调趋近，且 1s 内进入默认容差的 settled 态。
AURORA_TEST_CASE(critical_damping_no_overshoot_and_settles) {
    const aurora::SpringDescription crit{.stiffness = 100.0, .damping = 20.0, .mass = 1.0};
    const aurora::SpringSimulation sim{crit, 0.0, 1.0};

    for (int i = 1; i <= 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        AURORA_TEST_CHECK_LE(sim.value(t), 1.0);  // 全程不超过终点（无过冲）
    }
    AURORA_TEST_CHECK_NEAR(sim.value(0.1), 0.2642411, 1e-6);  // 1-(1+ω₀t)e^(-ω₀t)
    AURORA_TEST_CHECK_FALSE(sim.is_settled(0.1));
    AURORA_TEST_CHECK_TRUE(sim.is_settled(1.0));  // 位置与速度均进入 0.01 容差
}

/// @brief 欠阻尼出现一次过冲峰（≈1.73）后振荡衰减回落到终点。
AURORA_TEST_CASE(underdamped_overshoots_then_converges) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};  // ζ=0.1, ω₀=10
    const aurora::SpringSimulation sim{under, 0.0, 1.0};

    const double peak_t = std::numbers::pi / (10.0 * 0.99498743710662);  // π/ωd ≈ 0.3157
    AURORA_TEST_CHECK_GT(sim.value(peak_t), 1.0);  // 越过终点
    AURORA_TEST_CHECK_NEAR(sim.value(peak_t), 1.7293, 2e-3);
    AURORA_TEST_CHECK_NEAR(sim.value(5.0), 1.0, 0.02);  // 振荡衰减后回到目标附近
}

/// @brief 数值微分速度在过冲前后变号；初速度把「从目标出发」的轨迹推出正向位移。
AURORA_TEST_CASE(velocity_derivative_and_initial_velocity_shape_trajectory) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
    const aurora::SpringSimulation sim{under, 0.0, 1.0};
    // t=0 处数值微分跨越「t≤0 返回常量」的分界，误差有限但仍近零。
    AURORA_TEST_CHECK_NEAR(sim.velocity(0.0), 0.0, 0.01);
    AURORA_TEST_CHECK_GT(sim.velocity(0.05), 0.0);  // 冲向终点
    AURORA_TEST_CHECK_LT(sim.velocity(0.4), 0.0);  // 过冲后回落

    const aurora::SpringSimulation pushed{under, 0.0, 0.0, 5.0};  // 初速 5 单位/秒
    AURORA_TEST_CHECK_NEAR(pushed.value(0.0), 0.0, 1e-12);
    const double quarter_t = 1.57079632679489661923 / (10.0 * 0.99498743710662);  // (π/2)/ωd
    AURORA_TEST_CHECK_NEAR(pushed.value(quarter_t), 0.4291, 5e-3);  // 被初速推出 ≈0.43
}

/// @brief is_settled 要求位置与速度同时安静：位置已进容差但速度未静时不算 settled。
AURORA_TEST_CASE(settled_requires_position_and_velocity_quiet) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
    const aurora::SpringSimulation sim{under, 0.0, 1.0};
    AURORA_TEST_CHECK_FALSE(sim.is_settled(5.0));  // 位置 |Δ|≈0.007 已进容差，速度 ≈0.033 未静
    AURORA_TEST_CHECK_TRUE(sim.is_settled(5.0, 0.06));  // 放宽容差后双条件同时满足
    AURORA_TEST_CHECK_TRUE(sim.is_settled(12.0));  // 长时间后位置与速度均安静

    const aurora::SpringDescription crit{.stiffness = 100.0, .damping = 20.0, .mass = 1.0};
    const aurora::SpringSimulation crit_sim{crit, 0.0, 1.0};
    AURORA_TEST_CHECK_TRUE(crit_sim.is_settled(1.0));  // 临界阻尼 1s 即双双安静
}

}  // namespace aurora::test_cases::utest_spring
