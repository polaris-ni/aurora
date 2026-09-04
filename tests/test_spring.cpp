#include <cmath>
#include <cstdio>

#include "aurora/animation/spring.h"
#include "aurora/aurora.h"
#include "aurora/core/log.h"

#include "test_harness.h"

using aurora::SpringDescription;
using aurora::SpringSimulation;

static void test_description() {
    constexpr SpringDescription s;
    AURORA_TEST_CHECK_MSG(near_d(s.stiffness, 170.0), "default stiffness 170");
    AURORA_TEST_CHECK_MSG(near_d(s.damping, 26.0), "default damping 26");
    AURORA_TEST_CHECK_MSG(near_d(s.mass, 1.0), "default mass 1");
    const double w0 = s.natural_frequency();
    const double zeta = s.damping_ratio();
    AURORA_TEST_CHECK_MSG(near_d(w0, std::sqrt(170.0)), "natural_frequency = sqrt(k/m)");
    AURORA_TEST_CHECK_MSG(near_d(zeta, 26.0 / (2.0 * std::sqrt(170.0))), "damping_ratio formula");
}

static void test_simulation() {
    const SpringSimulation sim({ .stiffness = 200.0, .damping = 20.0, .mass = 1.0 }, 0.0, 100.0);
    AURORA_TEST_CHECK_MSG(near_d(sim.value(0.0), 0.0), "value(0) == start");
    AURORA_TEST_CHECK_MSG(near_d(sim.target(), 100.0), "target == end");
    // 充分时间后收敛到目标
    double t = 5.0;
    while (t < 20.0 && !sim.is_settled(t)) {
        t += 0.5;
    }
    AURORA_TEST_CHECK_MSG(sim.is_settled(t), "is_settled true after enough time");
    AURORA_TEST_CHECK_MSG(near_d(sim.value(t), 100.0, 0.05), "value converges to end");
}

static void test_damping_branches() {
    // 欠阻尼：低阻尼 -> 会过冲（value 超过 end）
    const SpringSimulation under({ .stiffness = 200.0, .damping = 5.0, .mass = 1.0 }, 0.0, 100.0);
    const double peak = under.value(0.3);
    AURORA_TEST_CHECK_MSG(peak > 100.0, "underdamped: overshoots past target");
    // 过阻尼：高阻尼 -> 单调收敛不过冲
    const SpringSimulation over({ .stiffness = 200.0, .damping = 60.0, .mass = 1.0 }, 0.0, 100.0);
    bool monotonic = true;
    double prev = under.value(0.0);
    // 步长 0.1 非二进制精确值，改写成整型计数会漂移采样时刻、改变断言取点，故保留浮点循环
    // NOLINTNEXTLINE(bugprone-float-loop-counter,clang-analyzer-security.FloatLoopCounter)
    for (double tt = 0.1; tt <= 3.0; tt += 0.1) {
        const double v = over.value(tt);
        if (v < prev - 1e-9) {
            monotonic = false;
        }
        prev = v;
    }
    AURORA_TEST_CHECK_MSG(monotonic, "overdamped: monotonic convergence");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== spring ===\n");
    test_description();
    test_simulation();
    test_damping_branches();
}
