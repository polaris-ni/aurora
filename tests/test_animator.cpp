// 目标源单元：animation/animator.h + src/aurora/animation/animator.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_anim.cpp
//   - test_animate.cpp
//   - test_animated_value.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include <iostream>

#include "aurora/animation/animator.h"
#include "aurora/animation/easing.h"
#include "aurora/animation/spring.h"
#include "aurora/animation/timeline.h"
#include "aurora/aurora.h"
#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/state/state.h"
#include "test_harness.h"

using aurora::AnimatedValue;
using aurora::AnimationController;
using aurora::AnimationStatus;
using aurora::Animator;
using aurora::Color;
using aurora::Curves;
using aurora::EdgeInsets;
using aurora::Keyframes;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::SpringDescription;
using aurora::SpringSimulation;
using aurora::State;
using aurora::Tween;
using aurora::TweenAnimation;

namespace sec_anim {

void run() {
    // 1) Curve 端点与中点
    {
        AURORA_TEST_CHECK(Curves::linear().transform(0.5) == 0.5);
        AURORA_TEST_CHECK(std::abs(Curves::ease_in().transform(0.5) - 0.125) < 1e-9);
        AURORA_TEST_CHECK(std::abs(Curves::ease_out().transform(0.5) - 0.875) < 1e-9);
        AURORA_TEST_CHECK(std::abs(Curves::ease_in_out().transform(0.5) - 0.5) < 1e-9);
        AURORA_TEST_CHECK(Curves::ease_out().transform(0.0) == 0.0);
        AURORA_TEST_CHECK(Curves::ease_out().transform(1.0) == 1.0);
        AURORA_LOG_INFO("test", "[1] Curve OK");
    }

    // 2) Tween: float / Point / Color
    {
        Tween tw(0.0F, 100.0F);
        AURORA_TEST_CHECK(std::abs(tw.value(0.5) - 50.0F) < 1e-6);
        AURORA_TEST_CHECK(tw.value(0.0) == 0.0F);
        AURORA_TEST_CHECK(std::abs(tw.value(1.0) - 100.0F) < 1e-6);

        Tween tp(Point{.x = 0, .y = 0}, Point{.x = 10, .y = 20});
        Point p = tp.value(0.5);
        AURORA_TEST_CHECK(p.x == 5.0F && p.y == 10.0F);

        Tween tc(Color::black(), Color::white());
        Color c = tc.value(1.0);
        AURORA_TEST_CHECK(c.m_r == 255 && c.m_g == 255 && c.m_b == 255 && c.m_a == 255);
        AURORA_LOG_INFO("test", "[2] Tween(float/Point/Color) OK");
    }

    // 3) Tween + 曲线：在控制器进度 0.5 处施加 easeIn（0.125）
    {
        Tween tw(0.0F, 100.0F, Curves::ease_in());
        AURORA_TEST_CHECK(std::abs(tw.value(0.5) - 12.5F) < 1e-6);
        AURORA_LOG_INFO("test", "[3] Tween+Curve OK");
    }

    // 4) Keyframes
    {
        Keyframes<float> kf({Keyframes<float>::Stop{.time = 0.0, .value = 0.0F},
                             Keyframes<float>::Stop{.time = 0.5, .value = 100.0F},
                             Keyframes<float>::Stop{.time = 1.0, .value = 50.0F}});
        AURORA_TEST_CHECK(kf.value(0.0) == 0.0F);
        AURORA_TEST_CHECK(std::abs(kf.value(0.5) - 100.0F) < 1e-6);
        AURORA_TEST_CHECK(std::abs(kf.value(0.25) - 50.0F) < 1e-6);  // 0→100 中点
        AURORA_TEST_CHECK(std::abs(kf.value(1.0) - 50.0F) < 1e-6);
        AURORA_LOG_INFO("test", "[4] Keyframes OK");
    }

    // 5) Spring：欠/临界/过阻尼
    {
        // 欠阻尼（明显过冲）
        SpringSimulation under(SpringDescription{.stiffness = 100.0, .damping = 2.0, .mass = 1.0}, 0.0, 100.0);
        AURORA_TEST_CHECK(std::abs(under.value(0.0) - 0.0) < 1e-9);
        AURORA_TEST_CHECK(under.value(0.25) > 100.0);  // 过冲（t≈0.25 处峰值 > 目标）
        AURORA_TEST_CHECK(under.value(0.5) < under.value(0.25));  // 之后回落
        AURORA_TEST_CHECK(std::abs(under.value(20.0) - 100.0) < 1.0);
        AURORA_TEST_CHECK(under.is_settled(20.0));

        // 临界阻尼（不过冲）
        SpringSimulation crit(SpringDescription{.stiffness = 100.0, .damping = 20.0, .mass = 1.0}, 0.0, 100.0);
        AURORA_TEST_CHECK(crit.value(0.5) < 100.0);
        AURORA_TEST_CHECK(std::abs(crit.value(10.0) - 100.0) < 1e-6);

        // 过阻尼（不过冲）
        SpringSimulation over(SpringDescription{.stiffness = 100.0, .damping = 40.0, .mass = 1.0}, 0.0, 100.0);
        AURORA_TEST_CHECK(over.value(0.5) < 100.0);
        AURORA_TEST_CHECK(std::abs(over.value(10.0) - 100.0) < 1e-6);
        AURORA_LOG_INFO("test", "[5] Spring(under/crit/over) OK");
    }

    // 6) AnimationController：正向 / 反向 / dirty
    {
        AnimationController c(1.0);
        c.forward();
        AURORA_TEST_CHECK(c.is_animating());
        c.tick(0.5);
        AURORA_TEST_CHECK(std::abs(c.value() - 0.5) < 1e-9);
        AURORA_TEST_CHECK(c.status() == AnimationStatus::Forward);
        AURORA_TEST_CHECK(c.dirty());
        c.tick(0.5);
        AURORA_TEST_CHECK(std::abs(c.value() - 1.0) < 1e-9);
        AURORA_TEST_CHECK(c.status() == AnimationStatus::Completed);
        AURORA_TEST_CHECK(c.dirty());
        c.clear_dirty();
        c.tick(0.5);  // 已完成，不变
        AURORA_TEST_CHECK(!c.dirty());
        AURORA_TEST_CHECK(c.status() == AnimationStatus::Completed);

        c.reverse();
        AURORA_TEST_CHECK(c.status() == AnimationStatus::Reverse);
        c.tick(0.5);
        AURORA_TEST_CHECK(std::abs(c.value() - 0.5) < 1e-9);
        c.tick(0.5);
        AURORA_TEST_CHECK(c.value() == 0.0);
        AURORA_TEST_CHECK(c.status() == AnimationStatus::Dismissed);
        AURORA_LOG_INFO("test", "[6] AnimationController OK");
    }

    // 7) Animator + bind -> State（含 easeIn 曲线）
    {
        State s(0.0);
        AnimationController c(1.0);
        Animator a;
        Tween tw(0.0, 100.0, Curves::ease_in());
        a.bind(c, tw, s);
        c.forward();

        a.tick(0.25);  // value 0.25 -> easeIn 0.015625 -> 1.5625
        AURORA_TEST_CHECK(std::abs(s.get() - 1.5625) < 1e-6);
        a.tick(0.25);  // value 0.5 -> easeIn 0.125 -> 12.5
        AURORA_TEST_CHECK(std::abs(s.get() - 12.5) < 1e-6);
        a.tick(0.5);  // 完成 -> 100
        AURORA_TEST_CHECK(std::abs(s.get() - 100.0) < 1e-6);
        a.tick(0.5);  // 空闲帧：不应改写
        AURORA_TEST_CHECK(std::abs(s.get() - 100.0) < 1e-6);
        AURORA_LOG_INFO("test", "[7] Animator+bind->State OK");
    }

    // 8) AnimatedValue 便捷封装
    {
        State s(Size{.width = 0, .height = 0});
        AnimatedValue av(s, Tween(Size{.width = 0, .height = 0}, Size{.width = 200, .height = 100}), 1.0);
        Animator a;
        av.attach(a);
        av.controller().forward();
        a.tick(0.5);
        AURORA_TEST_CHECK(std::abs(s.get().width - 100.0F) < 1e-6);
        AURORA_TEST_CHECK(std::abs(s.get().height - 50.0F) < 1e-6);
        AURORA_LOG_INFO("test", "[8] AnimatedValue OK");
    }

    AURORA_LOG_INFO("test", "ALL ANIMATION TESTS PASSED");
}
}  // namespace sec_anim

namespace sec_animate {

// ---- 1. animate() 手动 tick：线性补间在 duration 内从 begin 收敛到 end ----
static void test_animate_linear_converges() {
    State v{0.0};
    auto a = animate(v, Tween{0.0, 100.0, Curves::linear()}, 0.3);

    AURORA_TEST_CHECK(!a.is_completed());
    AURORA_TEST_CHECK(std::abs(v.get() - 0.0) < 1e-9);

    a.tick(0.1);  // t ≈ 0.333
    AURORA_TEST_CHECK(v.get() > 30.0 && v.get() < 40.0);

    a.tick(0.1);  // t ≈ 0.667
    AURORA_TEST_CHECK(v.get() > 60.0 && v.get() < 70.0);

    a.tick(0.1);  // 到达终点 1.0
    AURORA_TEST_CHECK(std::abs(v.get() - 100.0) < 1e-6);
    AURORA_TEST_CHECK(a.is_completed());
    AURORA_TEST_CHECK(std::abs(a.progress() - 1.0) < 1e-9);
}

// ---- 2. 曲线塑形：ease_in_out 在 t=0.25 处明显低于线性（验证曲线生效） ----
static void test_animate_curve_shapes() {
    // 线性参考：t=0.25 → 25
    State lin{0.0};
    auto a_lin = animate(lin, Tween{0.0, 100.0, Curves::linear()}, 1.0);
    a_lin.tick(0.25);
    AURORA_TEST_CHECK(lin.get() > 24.0 && lin.get() < 26.0);

    // ease_in_out 在 t=0.25 处 ≈ 4*t^3 = 0.0625 → 6.25，明显低于线性
    State eio{0.0};
    auto a_eio = animate(eio, Tween{0.0, 100.0, Curves::ease_in_out()}, 1.0);
    a_eio.tick(0.25);
    AURORA_TEST_CHECK(eio.get() < 15.0);  // 曲线把前期进度压低

    // 对称中点：ease_in_out 在 t=0.5 精确等于 0.5 → 50
    a_eio.tick(0.25);  // 累计 t=0.5
    AURORA_TEST_CHECK(std::abs(eio.get() - 50.0) < 1e-6);
}

// ---- 3. attach 接入 Animator 帧循环 + completed 回调触发 ----
static void test_animate_attach_and_completed() {
    State v{0.0F};
    Animator anim;
    bool done = false;

    auto a = animate(v, Tween{0.0F, 1.0F, Curves::linear()}, 0.2, anim);
    a.on_completed([&done]() -> void { done = true; });

    AURORA_TEST_CHECK(!a.is_completed());
    AURORA_TEST_CHECK(!done);

    anim.tick(0.1);  // t=0.5
    AURORA_TEST_CHECK(std::abs(v.get() - 0.5F) < 1e-6);
    AURORA_TEST_CHECK(!done);

    anim.tick(0.1);  // t=1.0，到达终点
    AURORA_TEST_CHECK(std::abs(v.get() - 1.0F) < 1e-6);
    AURORA_TEST_CHECK(a.is_completed());
    AURORA_TEST_CHECK(done);

    // 之后帧不再重复触发 completed
    done = false;
    anim.tick(0.1);
    AURORA_TEST_CHECK(!done);
}

// ---- 4. 句柄按值返回/拷贝共享同一驱动载荷（验证 pimpl 安全，不悬垂） ----
static void test_animate_handle_shared_payload() {
    State v{0.0};
    Animator anim;

    // 原句柄在块内构造并 attach，离开作用域后 animator 仍应安全驱动（payload 由 binding 持有）
    {
        auto const scoped = animate(v, Tween{0.0, 10.0, Curves::linear()}, 1.0, anim);
        // 拷贝一份句柄，验证共享 payload：通过拷贝读取 current/progress
        auto copy = scoped;
        copy.tick(0.5);  // 经拷贝驱动：t=0.5 → 5.0
        AURORA_TEST_CHECK(std::abs(v.get() - 5.0) < 1e-6);
        AURORA_TEST_CHECK(std::abs(copy.current() - 5.0) < 1e-6);
    }
    // scoped/copy 均已析构，但 animator 的 binding 仍持有 payload：继续驱动不应悬垂崩溃
    anim.tick(0.6);  // t=1.0 → 10.0
    AURORA_TEST_CHECK(std::abs(v.get() - 10.0) < 1e-6);
}

void run() {
    test_animate_linear_converges();
    test_animate_curve_shapes();
    test_animate_attach_and_completed();
    test_animate_handle_shared_payload();
}
}  // namespace sec_animate

namespace sec_animated_value {

// ---------- TweenAnimation<float> ----------

static void test_tween_animation_float() {
    TweenAnimation anim(0.0F);
    AURORA_TEST_CHECK(anim.get() == 0.0F);
    AURORA_TEST_CHECK(!anim.is_animating());

    anim.animate_to(1.0F, 1.0, Curves::linear());
    AURORA_TEST_CHECK(anim.is_animating());

    // 推进 0.5 秒（50%）
    anim.tick(0.5);
    const float mid = anim.get();
    AURORA_TEST_CHECK(mid > 0.4F && mid < 0.6F);  // 约 0.5

    // 推进到完成
    anim.tick(0.6);
    AURORA_TEST_CHECK(!anim.is_animating());
    AURORA_TEST_CHECK(std::abs(anim.get() - 1.0F) < 0.01F);
}

// ---------- TweenAnimation<Color> ----------

static void test_tween_animation_color() {
    const Color red(255, 0, 0, 255);
    const Color blue(0, 0, 255, 255);

    TweenAnimation anim(red);
    anim.animate_to(blue, 1.0, Curves::linear());

    // 50% 处应为紫色
    anim.tick(0.5);
    const Color mid = anim.get();
    AURORA_TEST_CHECK(mid.m_r > 100 && mid.m_r < 160);
    AURORA_TEST_CHECK(mid.m_b > 100 && mid.m_b < 160);

    // 完成处应为蓝色
    anim.tick(0.6);
    const Color end = anim.get();
    AURORA_TEST_CHECK(end.m_r == 0);
    AURORA_TEST_CHECK(end.m_b == 255);
}

// ---------- Tween<Rect> ----------

static void test_tween_rect() {
    const Rect a{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 50}};
    const Rect b{.origin = Point{.x = 10, .y = 20}, .size = Size{.width = 200, .height = 100}};

    const Rect mid = lerp(a, b, 0.5);
    AURORA_TEST_CHECK(std::abs(mid.origin.x - 5.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.origin.y - 10.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.size.width - 150.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.size.height - 75.0F) < 0.01F);
}

// ---------- Tween<EdgeInsets> ----------

static void test_tween_edge_insets() {
    const EdgeInsets a{.left = 0, .top = 0, .right = 0, .bottom = 0};
    const EdgeInsets b{.left = 10, .top = 20, .right = 30, .bottom = 40};

    const EdgeInsets mid = lerp(a, b, 0.5);
    AURORA_TEST_CHECK(std::abs(mid.left - 5.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.top - 10.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.right - 15.0F) < 0.01F);
    AURORA_TEST_CHECK(std::abs(mid.bottom - 20.0F) < 0.01F);
}

// ---------- Animator + AnimatedValue 集成 ----------

static void test_animator_bind() {
    State target{0.0F};
    AnimationController ctrl(1.0);
    const Tween tw(0.0F, 100.0F, Curves::linear());

    Animator animator;
    animator.bind(ctrl, tw, target);

    ctrl.forward(0.0);
    animator.tick(0.5);
    AURORA_TEST_CHECK(target.get() > 40.0F && target.get() < 60.0F);

    animator.tick(0.6);
    AURORA_TEST_CHECK(std::abs(target.get() - 100.0F) < 0.01F);
}

// ---------- as_signal 响应式 ----------

static void test_as_signal() {
    TweenAnimation anim(0.0F);
    State<float> const &sig = anim.as_signal();
    AURORA_TEST_CHECK(sig.get() == 0.0F);

    anim.animate_to(50.0F, 1.0, Curves::linear());
    anim.tick(1.1);  // 完成
    AURORA_TEST_CHECK(std::abs(sig.get() - 50.0F) < 0.01F);
}

static void run() {
    test_tween_animation_float();
    test_tween_animation_color();
    test_tween_rect();
    test_tween_edge_insets();
    test_animator_bind();
    test_as_signal();
}
}  // namespace sec_animated_value

AURORA_TEST() {
    sec_anim::run();
    sec_animate::run();
    sec_animated_value::run();
}
