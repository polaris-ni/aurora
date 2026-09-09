/// 测试类型: unit
/// 目标单元: include/aurora/animation/animator.h
/// 测试说明: 覆盖 AnimationController 构造夹取与正放/回放/复位/停止/帧推进状态机、Animator 驱动与 dirty
/// 门控绑定/注销、AnimatedValue 自驱与一次性 completed 回调、animate 工厂与 TweenAnimation 自持动画

#include "aurora/animation/animator.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_animator {

/// @brief 默认构造：进度 0、Dismissed、非动画；时长下限夹取为 1e-6；带初值构造不改变状态。
AURORA_TEST_CASE(controller_initial_state_and_duration_clamp) {
    const aurora::AnimationController c{0.3};
    AURORA_TEST_CHECK_NEAR(c.duration(), 0.3, 1e-12);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);
    AURORA_TEST_CHECK_TRUE(c.is_dismissed());
    AURORA_TEST_CHECK_FALSE(c.is_completed());
    AURORA_TEST_CHECK_FALSE(c.is_animating());
    AURORA_TEST_CHECK_FALSE(c.dirty());

    const aurora::AnimationController clamped{0.0};
    AURORA_TEST_CHECK_NEAR(clamped.duration(), 1e-6, 1e-15);  // 非正时长夹取为 1e-6

    // 初值 0.5 但状态仍是 Dismissed（构造不按值推断终态）。
    const aurora::AnimationController mid{1.0, 0.5};
    AURORA_TEST_CHECK_NEAR(mid.value(), 0.5, 1e-12);
    AURORA_TEST_CHECK_TRUE(mid.status() == aurora::AnimationStatus::Dismissed);
}

/// @brief forward 起步后 tick 按比例推进，到 1 钳制为 Completed；可从中间 restart；零时长一步完成。
AURORA_TEST_CASE(controller_forward_ticks_to_completed) {
    aurora::AnimationController c{1.0};
    c.forward();
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_TRUE(c.is_animating());

    c.tick(0.25);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.25, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.dirty());  // 本帧进度变化
    c.clear_dirty();
    AURORA_TEST_CHECK_FALSE(c.dirty());

    c.tick(0.25);
    c.tick(0.25);
    c.tick(0.25);
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Completed);
    AURORA_TEST_CHECK_TRUE(c.is_completed());
    AURORA_TEST_CHECK_FALSE(c.is_animating());
    AURORA_TEST_CHECK_TRUE(c.dirty());  // 到达终点的帧同样置脏

    c.tick(0.5);  // 终态后推进无效
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_FALSE(c.dirty());

    c.forward(0.5);  // 从中间重启
    AURORA_TEST_CHECK_NEAR(c.value(), 0.5, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);

    c.forward(1.0);  // 从 1 起步立即完成
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Completed);
    AURORA_TEST_CHECK_FALSE(c.is_animating());

    // 零时长控制器：一步 tick 即完成。
    aurora::AnimationController z{0.0};
    z.forward();
    z.tick(1e-6);
    AURORA_TEST_CHECK_TRUE(z.is_completed());
    AURORA_TEST_CHECK_NEAR(z.value(), 1.0, 1e-12);
}

/// @brief stop 冻结进度并把 <1 记为 Dismissed；reverse 反向推进到 0 记 Dismissed；reset 复位并夹取。
AURORA_TEST_CASE(controller_reverse_reset_stop_semantics) {
    aurora::AnimationController c{1.0};
    c.forward();
    c.tick(0.4);
    c.stop();
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);  // value<1 → Dismissed
    AURORA_TEST_CHECK_NEAR(c.value(), 0.4, 1e-12);  // 进度被保留
    c.tick(1.0);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.4, 1e-12);  // 停止后不再推进
    AURORA_TEST_CHECK_FALSE(c.dirty());

    c.reverse();
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);
    AURORA_TEST_CHECK_TRUE(c.is_animating());
    c.tick(0.2);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.2, 1e-12);
    c.tick(0.5);  // 反向越过 0 → 钳制并 Dismissed
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);

    c.reset(0.7);  // 中间值复位：静止但状态为 Dismissed
    AURORA_TEST_CHECK_NEAR(c.value(), 0.7, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);
    c.reset(2.0);  // 越界夹取到 1 → Completed
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Completed);
    c.reset(-1.0);  // 越界夹取到 0 → Dismissed
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);

    c.forward();
    c.tick(1.0);
    c.reverse();  // 从终点 1 反向仍为 Reverse
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);
    c.tick(2.0);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);
}

/// @brief Animator 推进所有已登记控制器（未播放者不动），帧末统一清 dirty，has_active 反映运行态。
AURORA_TEST_CASE(animator_drives_controllers_and_clears_dirty) {
    aurora::AnimationController a{1.0};
    aurora::AnimationController b{1.0};
    aurora::Animator animator;
    animator.drive(a);
    animator.drive(b);
    AURORA_TEST_CHECK_FALSE(animator.has_active());

    a.forward();
    AURORA_TEST_CHECK_TRUE(animator.has_active());

    animator.tick(0.5);
    AURORA_TEST_CHECK_NEAR(a.value(), 0.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(b.value(), 0.0, 1e-12);  // 未 forward 的控制器不受帧推进影响
    AURORA_TEST_CHECK_FALSE(a.dirty());  // Animator tick 末尾统一清 dirty

    animator.tick(0.5);
    AURORA_TEST_CHECK_TRUE(a.is_completed());
    AURORA_TEST_CHECK_FALSE(animator.has_active());
}

/// @brief bind/add_binding 仅在控制器 dirty 的帧写目标 State；空闲帧跳过写回。
AURORA_TEST_CASE(animator_bindings_write_only_on_dirty_frames) {
    aurora::State<double> target{0.0};
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 100.0}, target);

    int binding_calls = 0;
    int dirty_seen = 0;
    animator.add_binding([&c, &binding_calls, &dirty_seen]() -> void {
        ++binding_calls;
        if (c.dirty()) {
            ++dirty_seen;
        }
    });

    aurora::State<double> kf_target{0.0};
    aurora::AnimationController kc{1.0};
    animator.bind(kc, aurora::Keyframes<double>{{{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 10.0}}},
                  kf_target);

    c.forward();
    kc.forward();
    animator.tick(0.25);
    AURORA_TEST_CHECK_NEAR(target.get(), 25.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(kf_target.get(), 2.5, 1e-9);
    AURORA_TEST_CHECK_EQ(binding_calls, 1);
    AURORA_TEST_CHECK_EQ(dirty_seen, 1);

    animator.tick(0.0);  // 空闲帧：回调仍执行，但进度未变不写 State
    AURORA_TEST_CHECK_EQ(binding_calls, 2);
    AURORA_TEST_CHECK_EQ(dirty_seen, 1);
    AURORA_TEST_CHECK_NEAR(target.get(), 25.0, 1e-9);

    animator.tick(1.0);  // 双双到达终点
    AURORA_TEST_CHECK_NEAR(target.get(), 100.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(kf_target.get(), 10.0, 1e-9);
    AURORA_TEST_CHECK_EQ(dirty_seen, 2);
}

/// @brief remove 注销控制器及其绑定：不再推进也不再写 State；注销未登记控制器为无操作。
AURORA_TEST_CASE(animator_remove_detaches_controller_and_bindings) {
    aurora::State<double> s{0.0};
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 8.0}, s);
    c.forward();

    animator.remove(c);
    animator.tick(0.5);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);  // 已摘除，不再推进
    AURORA_TEST_CHECK_NEAR(s.get(), 0.0, 1e-12);  // 绑定随之失效，State 不被写入
    AURORA_TEST_CHECK_FALSE(animator.has_active());  // 尽管控制器自身仍在 Forward

    const aurora::AnimationController stranger{1.0};
    AURORA_TEST_CHECK_NO_THROW(animator.remove(stranger));  // 未登记过 → 无操作
}

/// @brief AnimatedValue 自驱 tick：插值写回 State，Completed 回调恰好触发一次；句柄可拷贝共享载荷。
AURORA_TEST_CASE(animated_value_self_tick_fires_completed_once) {
    aurora::State<double> s{0.0};
    aurora::AnimatedValue<double> av{s, aurora::Tween<double>{0.0, 1.0}, 1.0};
    int fired = 0;
    av.on_completed([&fired]() -> void { ++fired; });

    av.forward(0.0);
    AURORA_TEST_CHECK_TRUE(av.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_NEAR(av.progress(), 0.0, 1e-12);

    av.tick(0.4);
    AURORA_TEST_CHECK_NEAR(av.progress(), 0.4, 1e-12);
    AURORA_TEST_CHECK_NEAR(av.current(), 0.4, 1e-9);
    AURORA_TEST_CHECK_NEAR(s.get(), 0.4, 1e-9);
    AURORA_TEST_CHECK_EQ(fired, 0);

    av.tick(0.6);
    AURORA_TEST_CHECK_TRUE(av.is_completed());
    AURORA_TEST_CHECK_NEAR(av.progress(), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(s.get(), 1.0, 1e-9);
    AURORA_TEST_CHECK_EQ(fired, 1);

    av.tick(1.0);  // 终态后空闲帧：不重写、不重复触发
    AURORA_TEST_CHECK_EQ(fired, 1);

    AURORA_TEST_CHECK_NEAR(av.controller().value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(av.tween().end(), 1.0, 1e-12);

    const auto snapshot = av;  // 拷贝共享同一载荷
    AURORA_TEST_CHECK_NEAR(snapshot.progress(), 1.0, 1e-12);
}

/// @brief animate 工厂创建即起步（可自驱或 attach 到 Animator），TweenAnimation 自持状态独立推进。
AURORA_TEST_CASE(animate_factory_and_tween_animation_drive_state) {
    // 无 Animator：返回即 forward(0)，手动 tick 自驱。
    aurora::State<double> s{-1.0};
    auto handle = aurora::animate(s, aurora::Tween<double>{-1.0, 1.0}, 0.5);
    AURORA_TEST_CHECK_TRUE(handle.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_NEAR(handle.progress(), 0.0, 1e-12);
    handle.tick(0.5);
    AURORA_TEST_CHECK_TRUE(handle.is_completed());
    AURORA_TEST_CHECK_NEAR(s.get(), 1.0, 1e-9);

    // 带 Animator 重载：attach 后由帧循环驱动。
    aurora::State<double> s2{0.0};
    aurora::Animator animator;
    auto attached = aurora::animate(s2, aurora::Tween<double>{0.0, 8.0}, 2.0, animator);
    animator.tick(1.0);
    AURORA_TEST_CHECK_NEAR(s2.get(), 4.0, 1e-9);
    animator.tick(1.0);
    AURORA_TEST_CHECK_NEAR(s2.get(), 8.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(attached.is_completed());

    // TweenAnimation：自持 State，animate_to → tick → get。
    aurora::TweenAnimation<double> ta{0.0};
    AURORA_TEST_CHECK_FALSE(ta.is_animating());
    ta.animate_to(10.0, 1.0);
    AURORA_TEST_CHECK_TRUE(ta.is_animating());
    ta.tick(0.25);
    AURORA_TEST_CHECK_NEAR(ta.get(), 2.5, 1e-9);
    AURORA_TEST_CHECK_NEAR(ta.as_signal().get(), 2.5, 1e-9);
    ta.tick(0.75);
    AURORA_TEST_CHECK_NEAR(ta.get(), 10.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(ta.is_animating());
    ta.tick(1.0);  // 结束后 tick 无副作用
    AURORA_TEST_CHECK_NEAR(ta.get(), 10.0, 1e-9);
}

}  // namespace aurora::test_cases::utest_animator
