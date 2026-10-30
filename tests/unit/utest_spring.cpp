/// 测试类型: unit
/// 目标单元: include/aurora/animation/spring.h
/// 测试说明: 覆盖 SpringDescription 派生量、SpringSimulation 三种阻尼 regimes
/// 的端点/收敛、临界阻尼无过冲、欠阻尼过冲回落、初速度塑形、数值微分速度与 is_settled 的位置+速度双条件；
/// 以及 SpringAnimation 驱动封装——逐帧写 State 与定格停帧、on_settled 一次性、attach 后的活跃性与可注销、
/// 写出值夹取窗口、容差驱动停帧、时间缩放/冻结/reduce_motion、多个标量实例组合驱动向量

#include <numbers>

#include "aurora/animation/spring.h"
#include "framework/aurora_test.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_spring {

// 默认参数与自定义参数下的固有频率、阻尼比与公式一致（欠/临界/过阻尼取整值）。
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

// 三种阻尼 regimes 下：t≤0 返回起点，长时间后收敛到终点。
AURORA_TEST_CASE(simulation_matches_endpoints_across_damping_regimes) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
    const aurora::SpringDescription crit{.stiffness = 100.0, .damping = 20.0, .mass = 1.0};
    const aurora::SpringDescription over{.stiffness = 100.0, .damping = 40.0, .mass = 1.0};
    const aurora::SpringSimulation under_sim{under, 0.0, 1.0};
    const aurora::SpringSimulation crit_sim{crit, 0.0, 1.0};
    const aurora::SpringSimulation over_sim{over, 0.0, 1.0};

    AURORA_TEST_CHECK_EQ(under_sim.target(), 1.0);
    const aurora::SpringSimulation *regimes[] = {&under_sim, &crit_sim, &over_sim};
    for (const aurora::SpringSimulation *sim : regimes) {
        AURORA_TEST_TRACE("regime");
        AURORA_TEST_CHECK_NEAR(sim->value(0.0), 0.0, 1e-12);  // t=0 在起点
        AURORA_TEST_CHECK_NEAR(sim->value(-0.5), 0.0, 1e-12);  // 负时间同样返回起点
        AURORA_TEST_CHECK_NEAR(sim->value(10.0), 1.0, 1e-4);  // 长时间收敛到终点
    }
}

// 临界阻尼从起点到终点无过冲、单调趋近，且 1s 内进入默认容差的 settled 态。
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

// 欠阻尼出现一次过冲峰（≈1.73）后振荡衰减回落到终点。
AURORA_TEST_CASE(underdamped_overshoots_then_converges) {
    const aurora::SpringDescription under{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};  // ζ=0.1, ω₀=10
    const aurora::SpringSimulation sim{under, 0.0, 1.0};

    const double peak_t = std::numbers::pi / (10.0 * 0.99498743710662);  // π/ωd ≈ 0.3157
    AURORA_TEST_CHECK_GT(sim.value(peak_t), 1.0);  // 越过终点
    AURORA_TEST_CHECK_NEAR(sim.value(peak_t), 1.7293, 2e-3);
    AURORA_TEST_CHECK_NEAR(sim.value(5.0), 1.0, 0.02);  // 振荡衰减后回到目标附近
}

// 数值微分速度在过冲前后变号；初速度把「从目标出发」的轨迹推出正向位移。
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

// is_settled 要求位置与速度同时安静：位置已进容差但速度未静时不算 settled。
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

// ---- SpringAnimation 驱动封装 ----

namespace {
// 常用弹簧描述：默认参数近临界（ζ≈0.997，不过冲），欠阻尼参数用于验证回弹越界。
constexpr aurora::SpringDescription AURORA_DEFAULT_SPRING{.stiffness = 170.0, .damping = 22.0, .mass = 1.0};
constexpr aurora::SpringDescription AURORA_UNDERDAMPED_SPRING{.stiffness = 100.0, .damping = 2.0, .mass = 1.0};
constexpr double AURORA_FRAME_SECONDS = 1.0 / 64.0;  // 二进制精确的帧步长，便于跨倍率逐比特对齐累计时间
}  // namespace

// @brief 判据 20：逐帧逼近目标，is_settled 后定格于 target 并停帧；on_settled 恰触发一次，定格后无幽灵帧。
AURORA_TEST_CASE(spring_animation_converges_then_settles_exactly_once) {
    aurora::State<double> s{0.0};
    aurora::SpringAnimation spring{s};
    int settled = 0;
    spring.on_settled([&settled]() -> void { ++settled; });

    int writes = 0;
    const auto sub = s.subscribe([&](const double &) -> void { ++writes; });  // 订阅即交付首值

    spring.animate_to(100.0, AURORA_DEFAULT_SPRING);
    AURORA_TEST_CHECK_TRUE(spring.is_running());
    AURORA_TEST_CHECK_NEAR(spring.target(), 100.0, 1e-12);

    spring.tick(AURORA_FRAME_SECONDS);
    const double first = s.get();
    AURORA_TEST_CHECK_TRUE(first > 0.0 && first < 100.0);  // 中间帧在途中
    AURORA_TEST_CHECK_EQ(settled, 0);

    int guard = 0;
    while (spring.is_running() && guard < 2000) {
        spring.tick(AURORA_FRAME_SECONDS);
        ++guard;
    }
    AURORA_TEST_CHECK_FALSE(spring.is_running());
    AURORA_TEST_CHECK_NEAR(s.get(), 100.0, 1e-12);  // 定格写的是 target 本身，不是最后一次的逼近值
    AURORA_TEST_CHECK_EQ(settled, 1);
    const int writes_at_settle = writes;

    for (int i = 0; i < 20; ++i) {
        spring.tick(AURORA_FRAME_SECONDS);  // 定格后：不增长内部时间、不再写 State
    }
    AURORA_TEST_CHECK_EQ(settled, 1);
    AURORA_TEST_CHECK_EQ(writes, writes_at_settle);
}

// @brief 判据 22 的可观测形式：停帧由 is_settled（容差双条件）真实驱动——放宽容差即更早定格。
AURORA_TEST_CASE(spring_animation_settle_time_follows_tolerance) {
    auto run_until_settled = [](double tolerance) -> int {
        aurora::State<double> s{0.0};
        aurora::SpringAnimation spring{s};
        spring.animate_to(100.0, AURORA_DEFAULT_SPRING, 0.0, tolerance);
        int frames = 0;
        while (spring.is_running() && frames < 5000) {
            spring.tick(AURORA_FRAME_SECONDS);
            ++frames;
        }
        AURORA_TEST_CHECK_NEAR(s.get(), 100.0, 1e-12);  // 无论何时定格，终值都是 target
        return frames;
    };
    const int wide = run_until_settled(10.0);
    const int tight = run_until_settled(1e-4);
    AURORA_TEST_CHECK_GT(tight, wide);  // 停帧时机随收敛判据移动，而非偶然落点
    AURORA_TEST_CHECK_GT(wide, 1);  // 容差再宽也至少推进一帧
    AURORA_TEST_CHECK_LT(wide, 2 * tight);  // 同量级（防退化实现：一次性到位）
}

// @brief 未设夹取窗口时按原值写出（欠阻尼回弹可越界）；set_range 后写出值落在窗口内。
AURORA_TEST_CASE(spring_animation_writes_overshoot_unless_range_declared) {
    {
        aurora::State<double> s{0.0};
        aurora::SpringAnimation spring{s};
        spring.animate_to(100.0, AURORA_UNDERDAMPED_SPRING);
        bool crossed = false;
        for (int i = 0; i < 400 && spring.is_running(); ++i) {
            spring.tick(AURORA_FRAME_SECONDS);
            crossed = crossed || (s.get() > 100.0);
        }
        AURORA_TEST_CHECK_TRUE(crossed);  // 欠阻尼必然过冲，默认不夹（调用方要的就是这段回弹）
    }
    {
        aurora::State<double> s{0.0};
        aurora::SpringAnimation spring{s};
        spring.set_range(0.0, 100.0);
        spring.animate_to(100.0, AURORA_UNDERDAMPED_SPRING);
        bool clamped_hit = false;
        for (int i = 0; i < 400 && spring.is_running(); ++i) {
            spring.tick(AURORA_FRAME_SECONDS);
            AURORA_TEST_CHECK_LE(s.get(), 100.0);
            AURORA_TEST_CHECK_GE(s.get(), 0.0);
            clamped_hit = clamped_hit || (s.get() == 100.0);
        }
        AURORA_TEST_CHECK_TRUE(clamped_hit);  // 过冲帧被夹到上界
    }
}

// @brief 判据 21：attach 后在途期间 has_active 为真；定格后转假；Animator::remove(clock) 连绑定一并摘除。
AURORA_TEST_CASE(spring_animation_attach_keeps_active_and_is_removable) {
    aurora::Animator animator;

    aurora::State<double> s{0.0};
    aurora::SpringAnimation spring{s};
    spring.animate_to(1.0, AURORA_DEFAULT_SPRING);
    spring.attach(animator);
    AURORA_TEST_CHECK_TRUE(animator.has_active());  // 否则 wait_once 会深睡、把半途弹簧冻在原地

    animator.tick(AURORA_FRAME_SECONDS);
    AURORA_TEST_CHECK_TRUE(s.get() > 0.0);

    int guard = 0;
    while (spring.is_running() && guard < 5000) {
        animator.tick(AURORA_FRAME_SECONDS);
        ++guard;
    }
    AURORA_TEST_CHECK_FALSE(animator.has_active());  // 定格即停帧，无幽灵帧

    aurora::State<double> s2{0.0};
    aurora::SpringAnimation detached{s2};
    detached.attach(animator);
    detached.animate_to(50.0, AURORA_DEFAULT_SPRING);
    animator.tick(AURORA_FRAME_SECONDS);
    const double after_first = s2.get();
    AURORA_TEST_CHECK_TRUE(after_first > 0.0);
    animator.remove(detached.clock());
    for (int i = 0; i < 20; ++i) {
        animator.tick(AURORA_FRAME_SECONDS);  // 已注销：帧循环不再触碰该 State，也不再推进它
    }
    AURORA_TEST_CHECK_NEAR(s2.get(), after_first, 1e-12);
    AURORA_TEST_CHECK_FALSE(animator.has_active());
}

// @brief 判据 10/11 的弹簧行：同一动画时间轴总长下取值与倍率无关（无双层缩放）；冻结帧不推进不写值。
AURORA_TEST_CASE(spring_animation_scales_with_the_animator_and_freezes_on_zero) {
    aurora::State<double> a{0.0};
    aurora::State<double> b{0.0};
    aurora::SpringAnimation at_1x{a};
    aurora::SpringAnimation at_2x{b};
    at_1x.animate_to(100.0, AURORA_DEFAULT_SPRING);
    at_2x.animate_to(100.0, AURORA_DEFAULT_SPRING);

    aurora::Animator animator;
    const aurora::testing::ScopedCurrentAnimator scope{animator};  // 弹簧自驱取的是 current() 的倍率
    animator.set_time_scale(1.0);
    for (int i = 0; i < 16; ++i) {  // 16 × 1/64 × 1.0 = 0.25 秒动画时间轴
        at_1x.tick(AURORA_FRAME_SECONDS);
    }
    animator.set_time_scale(2.0);
    for (int i = 0; i < 8; ++i) {  // 8 × 1/64 × 2.0 = 0.25 秒——同一时间轴落点
        at_2x.tick(AURORA_FRAME_SECONDS);
    }
    AURORA_TEST_CHECK_NEAR(a.get(), b.get(), 1e-12);  // 倍率只折算步长；出现 4 倍差即双层缩放
    AURORA_TEST_CHECK_TRUE(at_1x.is_running() && at_2x.is_running());

    animator.set_time_scale(0.0);  // 冻结：时间轴不推进，State 保持当前值
    const double frozen = b.get();
    for (int i = 0; i < 5; ++i) {
        at_2x.tick(1.0);
    }
    AURORA_TEST_CHECK_TRUE(at_2x.is_running());
    AURORA_TEST_CHECK_NEAR(b.get(), frozen, 1e-12);

    animator.set_time_scale(1.0);  // 解冻从原处续跑到收敛，进度不丢
    int guard = 0;
    while (at_2x.is_running() && guard < 5000) {
        at_2x.tick(AURORA_FRAME_SECONDS);
        ++guard;
    }
    AURORA_TEST_CHECK_NEAR(b.get(), 100.0, 1e-12);
    AURORA_TEST_CHECK_GT(guard, 0);
}

// @brief 判据 24：reduce_motion 单帧落 target 并结束，与换封装前的手写弹簧一致。
AURORA_TEST_CASE(spring_animation_reduce_motion_settles_in_one_frame) {
    const aurora::testing::ScopedReduceMotion reduce{true};
    aurora::State<double> s{0.0};
    aurora::SpringAnimation spring{s};
    int settled = 0;
    spring.on_settled([&settled]() -> void { ++settled; });
    spring.animate_to(42.0, AURORA_UNDERDAMPED_SPRING, 500.0);

    spring.tick(AURORA_FRAME_SECONDS);
    AURORA_TEST_CHECK_NEAR(s.get(), 42.0, 1e-12);
    AURORA_TEST_CHECK_FALSE(spring.is_running());
    AURORA_TEST_CHECK_EQ(settled, 1);

    spring.tick(AURORA_FRAME_SECONDS);  // 已结束的弹簧不重复触发
    AURORA_TEST_CHECK_EQ(settled, 1);
}

// @brief 判据 27：只做标量——组合两个实例即可驱动二维位移，互不干扰且各自收敛。
AURORA_TEST_CASE(two_scalar_springs_compose_into_a_vector_offset) {
    aurora::State<double> x{0.0};
    aurora::State<double> y{0.0};
    aurora::SpringAnimation sx{x};
    aurora::SpringAnimation sy{y};
    int settled_x = 0;
    int settled_y = 0;
    sx.on_settled([&settled_x]() -> void { ++settled_x; });
    sy.on_settled([&settled_y]() -> void { ++settled_y; });

    sx.animate_to(10.0, AURORA_DEFAULT_SPRING);
    sy.animate_to(-4.0, AURORA_DEFAULT_SPRING);
    for (int i = 0; i < 5000 && (sx.is_running() || sy.is_running()); ++i) {
        sx.tick(AURORA_FRAME_SECONDS);
        sy.tick(AURORA_FRAME_SECONDS);
    }
    AURORA_TEST_CHECK_NEAR(x.get(), 10.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(y.get(), -4.0, 1e-12);
    AURORA_TEST_CHECK_EQ(settled_x, 1);
    AURORA_TEST_CHECK_EQ(settled_y, 1);
}

// @brief 未起播 / 句柄拷贝共享载荷：拷贝后任一句柄起播都写同一来源，收敛判定只跑一次。
AURORA_TEST_CASE(spring_animation_handle_copies_share_the_payload) {
    aurora::State<double> s{0.0};
    aurora::SpringAnimation spring{s};
    // 句柄拷贝（载荷是 shared_ptr）正是本例的被测对象，不是可省略的中转变量：按建议直接复用
    // `spring` 会让这条用例失去要验证的语义，故只能就地抑制该检查项。
    aurora::SpringAnimation alias = spring;  // NOLINT(performance-unnecessary-copy-initialization)

    AURORA_TEST_CHECK_FALSE(spring.is_running());  // 未起播时 tick 为 no-op
    spring.tick(AURORA_FRAME_SECONDS);
    AURORA_TEST_CHECK_NEAR(s.get(), 0.0, 1e-12);

    alias.animate_to(8.0, AURORA_DEFAULT_SPRING);
    AURORA_TEST_CHECK_TRUE(spring.is_running());  // 同一载荷
    int guard = 0;
    while (spring.is_running() && guard < 5000) {
        alias.tick(AURORA_FRAME_SECONDS);
        ++guard;
    }
    AURORA_TEST_CHECK_NEAR(s.get(), 8.0, 1e-12);
}

}  // namespace aurora::test_cases::utest_spring
