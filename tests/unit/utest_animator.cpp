/// 测试类型: unit
/// 目标单元: include/aurora/animation/animator.h
/// 测试说明: 覆盖 AnimationController 构造夹取与正放/回放/复位/停止/帧推进状态机、循环模式的同向重启 /
/// 往复 / 有限计数 / 与方向正交 / completed 只在耗尽那帧触发、`set_progress` 的定位语义与外部脏位当帧写回、
/// Animator 驱动与 dirty 门控绑定/注销、应用内全局时间缩放（倍率折半 / 冻结不置脏不写回 / 非法值夹取 /
/// 四条推进路径同倍率 / 单一漏斗不叠加 / 无 current 时恒等 / reduce_motion 优先）、AnimatedValue 自驱与
/// 一次性 completed 回调、animate 工厂与 TweenAnimation 自持动画、
/// TimelinePlayer 多轨道编排（起播初始化 / 正放值序 / 反向镜像倒放 / 中断续播 / completed 一次性 /
/// attach 生命周期 / 空 spec 与越界槽位防御）、关键帧绑定的分段曲线应用

#include <limits>

#include "aurora/animation/animator.h"
#include "framework/aurora_test.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_animator {

// @brief 默认构造：进度 0、Dismissed、非动画；时长下限夹取为 1e-6；带初值构造不改变状态。
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

// @brief forward 起步后 tick 按比例推进，到 1 钳制为 Completed；可从中间 restart；零时长一步完成。
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

// @brief stop 冻结进度并把 <1 记为 Dismissed；reverse 反向推进到 0 记 Dismissed；reset 复位并夹取。
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

// @brief Animator 推进所有已登记控制器（未播放者不动），帧末统一清 dirty，has_active 反映运行态。
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

// @brief bind/add_binding 仅在控制器 dirty 的帧写目标 State；空闲帧跳过写回。
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

// @brief remove 注销控制器及其绑定：不再推进也不再写 State；注销未登记控制器为无操作。
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

// @brief AnimatedValue 自驱 tick：插值写回 State，Completed 回调恰好触发一次；句柄可拷贝共享载荷。
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

// @brief animate 工厂创建即起步（可自驱或 attach 到 Animator），TweenAnimation 自持状态独立推进。
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

// ---- TimelinePlayer ----

/// @brief 编排测试夹具：sequence(0.2 + 0.3)，轨道 0 = 0→10，轨道 1 = 0→1。
/// 总时长 0.5：轨道 0 区间 [0, 0.4]，轨道 1 区间 [0.4, 1.0]。
namespace {
auto make_two_segment_timeline() -> aurora::TimelineResolved {
    return aurora::TimelineSpec::sequence().add(0.2).add(0.3).build();
}
}  // namespace

AURORA_TEST_CASE(timeline_player_forward_initializes_and_advances) {
    aurora::State<double> a{5.0};  // 初始脏值：起播应统一初始化到 begin 值
    aurora::State<double> b{7.0};
    aurora::TimelinePlayer player{make_two_segment_timeline()};
    player.track<double>(0, aurora::Tween<double>{0.0, 10.0}, a);
    player.track<double>(1, aurora::Tween<double>{0.0, 1.0}, b);

    // 起播：全部轨道立刻落 begin 值（消除脏旧值）。
    player.forward(0.0);
    AURORA_TEST_CHECK_NEAR(a.get(), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(b.get(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(player.is_animating());

    // 推进 0.1s（主进度 0.2）：轨道 0 局部 t=0.5 → 5；轨道 1 未开始 → 0。
    player.tick(0.1);
    AURORA_TEST_CHECK_NEAR(a.get(), 5.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(b.get(), 0.0, 1e-12);

    // 推进到 0.25s（主进度 0.5）：轨道 0 完成 → 10；轨道 1 局部 t = (0.5-0.4)/0.6 ≈ 0.1667。
    player.tick(0.15);
    AURORA_TEST_CHECK_NEAR(a.get(), 10.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(b.get(), (0.5 - 0.4) / 0.6, 1e-9);

    // 推到端点：completed + 一次性回调。
    int completed_count = 0;
    player.on_completed([&completed_count] { ++completed_count; });
    player.tick(0.25);
    AURORA_TEST_CHECK_TRUE(player.is_completed());
    AURORA_TEST_CHECK_NEAR(b.get(), 1.0, 1e-9);
    AURORA_TEST_CHECK_EQ(completed_count, 1);
    // 重复到达不重复触发。
    player.tick(0.1);
    AURORA_TEST_CHECK_EQ(completed_count, 1);
}

AURORA_TEST_CASE(timeline_player_reverse_mirrors_and_interrupt_resume) {
    aurora::State<double> a{0.0};
    aurora::State<double> b{0.0};
    aurora::TimelinePlayer player{make_two_segment_timeline()};
    player.track<double>(0, aurora::Tween<double>{0.0, 10.0}, a);
    player.track<double>(1, aurora::Tween<double>{0.0, 1.0}, b);
    player.forward(0.0);

    // 正放到主进度 0.5（轨道 0 完成、轨道 1 进行中）。
    player.tick(0.25);
    AURORA_TEST_CHECK_NEAR(a.get(), 10.0, 1e-9);
    const double b_mid = b.get();
    AURORA_TEST_CHECK_NEAR(b_mid, (0.5 - 0.4) / 0.6, 1e-9);

    // 中断：stop 保持当前进度（不重置）。
    player.stop();
    AURORA_TEST_CHECK_NEAR(player.progress(), 0.5, 1e-12);
    AURORA_TEST_CHECK_FALSE(player.is_animating());

    // 续播：forward(-1) 从当前进度续。
    player.forward();
    AURORA_TEST_CHECK_NEAR(player.progress(), 0.5, 1e-12);
    player.tick(0.05);
    AURORA_TEST_CHECK_TRUE(a.get() > 10.0 - 1e-9);  // 轨道 0 已完成保持端点

    // 反向镜像：reverse 从当前进度倒放——轨道 1 先退，轨道 0 后退。
    player.tick(0.05);  // 先推到 ~0.6
    const double progress_before_reverse = player.progress();
    player.reverse();
    AURORA_TEST_CHECK_NEAR(player.progress(), progress_before_reverse, 1e-12);  // 进度保留
    player.tick(0.25);  // 倒退 0.5s → 主进度回 ~0.1
    AURORA_TEST_CHECK_TRUE(player.status() == aurora::AnimationStatus::Reverse ||
                           player.status() == aurora::AnimationStatus::Dismissed);
    // 轨道 0 倒放中（< 10），轨道 1 未开始（0）。
    AURORA_TEST_CHECK_TRUE(a.get() < 10.0);
    AURORA_TEST_CHECK_NEAR(b.get(), 0.0, 1e-12);

    // 倒放到头：Dismissed + 全轨回 begin。
    player.tick(0.5);
    AURORA_TEST_CHECK_TRUE(player.status() == aurora::AnimationStatus::Dismissed);
    AURORA_TEST_CHECK_NEAR(a.get(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(b.get(), 0.0, 1e-9);
}

AURORA_TEST_CASE(timeline_player_attach_survives_handle_scope) {
    aurora::Animator animator;
    aurora::State<double> a{0.0};
    aurora::State<double> b{0.0};
    {
        aurora::TimelinePlayer player{make_two_segment_timeline()};
        player.track<double>(0, aurora::Tween<double>{0.0, 10.0}, a);
        player.track<double>(1, aurora::Tween<double>{0.0, 1.0}, b);
        player.forward(0.0);
        player.attach(animator);
    }  // 句柄离开作用域：帧循环安全驱动（shared_ptr 载荷）。
    animator.tick(0.5);
    AURORA_TEST_CHECK_NEAR(a.get(), 10.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(b.get(), 1.0, 1e-9);
}

AURORA_TEST_CASE(timeline_player_empty_spec_and_out_of_range_slot) {
    // 空 spec：无轨道、播放瞬时完成（时长夹取 1e-6）。
    aurora::TimelinePlayer player{aurora::TimelineSpec::sequence().build()};
    aurora::State<double> a{3.0};
    player.track<double>(99, aurora::Tween<double>{0.0, 1.0}, a);  // 越界槽位：无操作
    player.forward(0.0);
    player.tick(0.1);
    AURORA_TEST_CHECK_NEAR(a.get(), 3.0, 1e-12);  // 未被触碰
    AURORA_TEST_CHECK_TRUE(player.is_completed() || player.is_animating());

    // 同槽位重复绑定：覆盖前值（旧 Tween 不再驱动）。
    aurora::State<double> x{0.0};
    aurora::TimelinePlayer p2{make_two_segment_timeline()};
    p2.track<double>(0, aurora::Tween<double>{0.0, 10.0}, x);
    p2.track<double>(0, aurora::Tween<double>{100.0, 200.0}, x);  // 覆盖
    p2.forward(0.0);
    AURORA_TEST_CHECK_NEAR(x.get(), 100.0, 1e-9);  // 新 Tween 的 begin 值
    p2.tick(0.1);
    AURORA_TEST_CHECK_NEAR(x.get(), 150.0, 1e-9);  // 新 Tween 推进
}

// ---- 循环播放（PlaybackMode::Repeat） ----

// @brief 判据 1：`repeat(-1,false)` 到达 1.0 后同向重启到 0 续跑；`repeat(0,...)` 与 `-1` 同义（不是「播 0 次」）。
AURORA_TEST_CASE(repeat_infinite_restarts_forward_at_zero_and_zero_count_means_infinite) {
    aurora::AnimationController c{1.0};
    c.repeat(-1, false);
    AURORA_TEST_CHECK_TRUE(c.mode() == aurora::PlaybackMode::Repeat);
    AURORA_TEST_CHECK_TRUE(c.is_repeating());
    AURORA_TEST_CHECK_FALSE(c.is_animating());  // repeat 只登记模式，不起播

    c.forward();
    c.tick(0.6);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.6, 1e-12);
    c.clear_dirty();

    c.tick(0.6);  // 越过 1.0 → 钳到端点后重启到 0，方向保持 Forward
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_FALSE(c.is_completed());
    AURORA_TEST_CHECK_TRUE(c.dirty());  // 重启帧进度有跳变，照常置脏

    c.clear_dirty();
    c.tick(0.6);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.6, 1e-12);

    // count == 0 与 -1 同义：到达端点仍是无限续播。
    aurora::AnimationController z{1.0};
    z.repeat(0);
    z.forward();
    z.tick(1.0);
    AURORA_TEST_CHECK_NEAR(z.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(z.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_TRUE(z.is_repeating());
}

// @brief 判据 2：`repeat(-1,true)` 在两端翻方向往复，进度停在被到达的端点，每次翻转帧置脏、不卡死单端。
AURORA_TEST_CASE(repeat_alternate_oscillates_between_both_endpoints) {
    aurora::AnimationController c{1.0};
    c.repeat(-1, true);
    c.forward();

    c.tick(1.0);  // 到 1.0 → 只翻方向，值留在端点
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);
    AURORA_TEST_CHECK_TRUE(c.dirty());
    c.clear_dirty();

    c.tick(0.4);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.6, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);

    c.tick(1.0);  // 越过 0.0 → 翻回 Forward
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_TRUE(c.dirty());
    c.clear_dirty();

    c.tick(0.5);  // 往复不卡死：仍能从另一端续跑
    AURORA_TEST_CHECK_NEAR(c.value(), 0.5, 1e-12);
}

// @brief 判据 3：有限计数在最后到达的端点置终态并停；`repeat(1,false)` 与既有单次播放逐位等价。
AURORA_TEST_CASE(repeat_finite_count_stops_at_last_reached_endpoint) {
    aurora::AnimationController c{1.0};
    c.repeat(2, false);
    c.forward();

    c.tick(1.0);  // 第 1 次到达端点 → 重启
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.is_animating());
    AURORA_TEST_CHECK_TRUE(c.is_repeating());

    c.clear_dirty();
    c.tick(1.0);  // 第 2 次到达 → 计数耗尽，停在 1.0 并置 Completed
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.is_completed());
    AURORA_TEST_CHECK_FALSE(c.is_repeating());
    AURORA_TEST_CHECK_FALSE(c.is_animating());

    c.clear_dirty();
    c.tick(1.0);  // 终态后不再置脏（无幽灵帧）
    AURORA_TEST_CHECK_FALSE(c.dirty());

    // n == 1 与一次性播放逐位等价。
    aurora::AnimationController once{1.0};
    once.forward();
    once.tick(0.25);
    aurora::AnimationController single{1.0};
    single.repeat(1, false);
    single.forward();
    single.tick(0.25);
    AURORA_TEST_CHECK_NEAR(single.value(), once.value(), 0.0);
    AURORA_TEST_CHECK_TRUE(single.status() == once.status());
    once.tick(0.75);
    single.tick(0.75);
    AURORA_TEST_CHECK_NEAR(single.value(), once.value(), 0.0);
    AURORA_TEST_CHECK_TRUE(single.status() == once.status());
    AURORA_TEST_CHECK_TRUE(single.is_completed());
}

// @brief 判据 4：repeat 与 forward/reverse 正交；reset / stop 把模式清回 Once，其余行为不变。
AURORA_TEST_CASE(repeat_is_orthogonal_to_direction_and_cleared_by_reset_and_stop) {
    aurora::AnimationController c{1.0};
    c.forward();
    c.repeat(-1, true);  // 先 forward 再 repeat：不覆盖方向
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);
    AURORA_TEST_CHECK_TRUE(c.is_repeating());
    c.tick(1.0);  // 往复确实生效
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);

    c.reset(0.2);  // 复位即退出循环
    AURORA_TEST_CHECK_FALSE(c.is_repeating());
    AURORA_TEST_CHECK_NEAR(c.value(), 0.2, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);

    c.repeat(-1, false);  // 先 repeat 再 forward：不清模式
    c.forward(0.0);
    AURORA_TEST_CHECK_TRUE(c.is_repeating());
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);

    c.stop();  // 停止同样退出循环（无限循环的既定出口）
    AURORA_TEST_CHECK_FALSE(c.is_repeating());
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
}

// @brief 判据 5：循环期间 completed 只在有限计数耗尽那一帧触发一次；无限循环全程不触发。
AURORA_TEST_CASE(repeat_completed_callback_fires_only_when_count_exhausts) {
    aurora::Animator animator;

    int fired_infinite = 0;
    aurora::State<double> si{9.0};
    aurora::AnimatedValue<double> infinite{si, aurora::Tween<double>{0.0, 1.0}, 1.0};
    infinite.on_completed([&fired_infinite]() -> void { ++fired_infinite; });
    infinite.controller().repeat(-1, false);
    infinite.forward(0.0);
    infinite.attach(animator);
    for (int i = 0; i < 8; ++i) {
        animator.tick(1.0);  // 每帧正好走完一整轮并重启
    }
    AURORA_TEST_CHECK_EQ(fired_infinite, 0);
    AURORA_TEST_CHECK_NEAR(si.get(), 0.0, 1e-12);  // 端点即重启到 0，写回的是重启后的进度

    int fired_finite = 0;
    aurora::State<double> sf{0.0};
    aurora::AnimatedValue<double> finite{sf, aurora::Tween<double>{0.0, 1.0}, 1.0};
    finite.on_completed([&fired_finite]() -> void { ++fired_finite; });
    finite.controller().repeat(2, false);
    finite.forward(0.0);
    finite.attach(animator);
    animator.tick(1.0);  // 第 1 次端点：重启，不触发
    AURORA_TEST_CHECK_EQ(fired_finite, 0);
    animator.tick(1.0);  // 第 2 次端点：耗尽 → Completed → 恰触发一次
    AURORA_TEST_CHECK_EQ(fired_finite, 1);
    AURORA_TEST_CHECK_NEAR(sf.get(), 1.0, 1e-9);
    animator.tick(1.0);
    AURORA_TEST_CHECK_EQ(fired_finite, 1);  // 终态空闲帧不重复触发
}

// @brief 判据 6：reduce_motion 优先于循环——一次落到本次目标端点并置终态，不进入循环。
AURORA_TEST_CASE(reduce_motion_short_circuits_repeat_instead_of_looping) {
    const aurora::testing::ScopedReduceMotion reduce{true};

    aurora::AnimationController c{1.0};
    c.repeat(-1, true);
    c.forward();
    c.tick(0.3);
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.is_completed());
    AURORA_TEST_CHECK_FALSE(c.is_repeating());  // 一次到位即止
    AURORA_TEST_CHECK_TRUE(c.dirty());
    c.clear_dirty();
    c.tick(0.3);
    AURORA_TEST_CHECK_FALSE(c.dirty());  // 终态后不再产生循环帧

    // 反向播放落起点（端点按本次方向选择）。
    aurora::AnimationController r{1.0};
    r.repeat(-1, false);
    r.forward(0.5);
    r.reverse();
    r.tick(0.3);
    AURORA_TEST_CHECK_NEAR(r.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(r.status() == aurora::AnimationStatus::Dismissed);
}

// @brief 判据 7：无限循环在途期间 `has_active()` 恒真（常驻活跃帧），stop 后转假。
AURORA_TEST_CASE(infinite_repeat_keeps_animator_active_until_stop) {
    aurora::Animator animator;
    aurora::AnimationController c{1.0};
    animator.drive(c);
    c.repeat(-1, true);
    c.forward();
    for (int i = 0; i < 6; ++i) {
        animator.tick(0.4);
        AURORA_TEST_CHECK_TRUE(animator.has_active());
    }
    c.stop();
    AURORA_TEST_CHECK_FALSE(animator.has_active());
    AURORA_TEST_CHECK_FALSE(c.is_repeating());
}

// ---- 交互式进度 setter（set_progress） ----

// @brief 判据 15/18：只定位、夹入 [0,1]、不改 status_ 也不清循环模式；与 reset 的按值改状态明确区分。
AURORA_TEST_CASE(set_progress_positions_without_touching_status_or_mode) {
    aurora::AnimationController c{1.0};
    c.repeat(-1, true);

    c.set_progress(0.4);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.4, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);  // 静止语义不变
    AURORA_TEST_CHECK_TRUE(c.is_repeating());
    AURORA_TEST_CHECK_FALSE(c.is_animating());

    c.set_progress(5.0);  // 越界夹取到 1.0，但不因落在终点而改写 status
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);
    c.set_progress(-5.0);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);

    c.forward(0.5);  // 播放中调用 = 重定位后从新进度续跑
    c.set_progress(0.2);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Forward);
    c.tick(0.3);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.5, 1e-12);

    c.reset(1.0);  // 对照：reset 按值改写 status_ 并清循环模式
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Completed);
    AURORA_TEST_CHECK_FALSE(c.is_repeating());
}

// @brief 判据 16/17：外部脏位活到帧末——手势在帧边界前写入，同一帧绑定阶段即写回目标 State。
AURORA_TEST_CASE(set_progress_writes_back_through_binding_in_the_same_frame) {
    aurora::State<double> s{0.0};
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 100.0}, s);

    AURORA_TEST_CHECK_FALSE(animator.has_active());  // 静止控制器只定位，不使帧循环转入活跃
    c.set_progress(0.25);  // 手势写入（帧边界前）
    animator.tick(0.0);  // 本帧不推进，仅消费外部脏位
    AURORA_TEST_CHECK_NEAR(s.get(), 25.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(c.dirty());  // 帧末把推进脏与外部脏一并清

    c.set_progress(0.5);
    animator.tick(0.0);
    AURORA_TEST_CHECK_NEAR(s.get(), 50.0, 1e-9);

    c.forward();  // 起步后同帧推进脏与外部脏并存，绑定拿到的是推进后的进度
    AURORA_TEST_CHECK_TRUE(animator.has_active());
    animator.tick(0.1);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(s.get(), 60.0, 1e-9);

    animator.set_time_scale(0.0);  // 冻结只冻结推进步长，定位语义不受影响
    c.set_progress(0.8);
    animator.tick(0.1);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.8, 1e-12);
    AURORA_TEST_CHECK_NEAR(s.get(), 80.0, 1e-9);
}

// @brief 判据 19：拖拽逐事件定位 → 松手由 forward / reverse 接管收尾，全程不借道 reset。
AURORA_TEST_CASE(gesture_drag_then_forward_or_reverse_takes_over) {
    aurora::State<double> drawer{0.0};
    aurora::AnimationController c{0.3};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 1.0}, drawer);

    auto drag = [&](double p) -> void {
        c.set_progress(p);
        animator.tick(0.0);
        AURORA_TEST_CHECK_NEAR(drawer.get(), p, 1e-9);
    };
    drag(0.10);
    drag(0.32);
    drag(0.55);
    AURORA_TEST_CHECK_FALSE(c.is_animating());
    AURORA_TEST_CHECK_FALSE(animator.has_active());

    c.forward();  // 松手收尾：从当前进度续跑
    AURORA_TEST_CHECK_NEAR(c.value(), 0.55, 1e-12);
    animator.tick(0.5);
    AURORA_TEST_CHECK_TRUE(c.is_completed());
    AURORA_TEST_CHECK_NEAR(drawer.get(), 1.0, 1e-9);

    c.set_progress(0.4);  // 再次拖拽定位（status 仍是 Completed，不被改写）
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Completed);
    c.reverse();  // 反向收尾
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Reverse);
    animator.tick(0.5);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(drawer.get(), 0.0, 1e-9);
}

// ---- 应用内全局时间缩放 ----

// @brief 判据 8：倍率 2.0 下到位帧数恰为一半；0.0 冻结；负值 / NaN / inf 一律夹取为 0.0；默认 1.0。
AURORA_TEST_CASE(time_scale_halves_frames_and_clamps_invalid_values) {
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    animator.drive(c);
    AURORA_TEST_CHECK_NEAR(animator.time_scale(), 1.0, 0.0);

    int frames_1x = 0;
    c.forward();
    while (!c.is_completed() && frames_1x < 100) {
        animator.tick(0.25);
        ++frames_1x;
    }
    AURORA_TEST_CHECK_EQ(frames_1x, 4);

    animator.set_time_scale(2.0);
    c.reset();
    c.forward();
    int frames_2x = 0;
    while (!c.is_completed() && frames_2x < 100) {
        animator.tick(0.25);
        ++frames_2x;
    }
    AURORA_TEST_CHECK_EQ(frames_2x, 2);

    animator.set_time_scale(0.0);  // 冻结：一步不推进
    c.reset();
    c.forward();
    animator.tick(1.0);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_FALSE(c.dirty());

    animator.set_time_scale(-1.0);
    AURORA_TEST_CHECK_NEAR(animator.time_scale(), 0.0, 1e-12);
    animator.set_time_scale(1.0);
    animator.set_time_scale(std::numeric_limits<double>::quiet_NaN());
    AURORA_TEST_CHECK_NEAR(animator.time_scale(), 0.0, 1e-12);
    animator.set_time_scale(1.0);
    animator.set_time_scale(std::numeric_limits<double>::infinity());
    AURORA_TEST_CHECK_NEAR(animator.time_scale(), 0.0, 1e-12);
    animator.set_time_scale(0.5);
    AURORA_TEST_CHECK_NEAR(animator.time_scale(), 0.5, 1e-12);
}

// @brief 判据 9：缩放只改步长——时长、曲线、目标终值不变，还原 1.0 后按原速率续跑且无残留。
AURORA_TEST_CASE(scaling_preserves_duration_curve_and_final_value_without_residue) {
    aurora::State<double> s{0.0};
    aurora::AnimationController c{2.0};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 100.0, aurora::Curves::ease_in_quad()}, s);
    const double duration_before = c.duration();

    animator.set_time_scale(0.25);
    c.forward();
    animator.tick(0.5);  // 折算步长 0.125 → 进度 0.0625 → 二次缓入 0.00390625 → 0.390625
    AURORA_TEST_CHECK_NEAR(c.duration(), duration_before, 1e-12);  // 时长不被改写
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0625, 1e-12);
    AURORA_TEST_CHECK_NEAR(s.get(), 100.0 * 0.0625 * 0.0625, 1e-9);

    animator.set_time_scale(1.0);  // 还原：后续帧按原速率推进
    int guard = 0;
    while (!c.is_completed() && guard < 100) {
        animator.tick(0.5);
        ++guard;
    }
    AURORA_TEST_CHECK_EQ(guard, 4);  // 余 0.9375，每帧 0.25 → 4 帧
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(s.get(), 100.0, 1e-9);  // 终值与不缩放时一致
}

// @brief 判据 10 的 Animator 侧覆盖矩阵：四条推进路径在同一倍率下到位帧数都恰为 1.0 倍率的一半（无双层缩放）。
AURORA_TEST_CASE(every_progress_path_scales_by_exactly_the_multiplier) {
    constexpr double tick_step = 0.25;
    auto frames_until = [](const auto &done, const auto &step) -> int {
        int n = 0;
        while (!done() && n < 64) {
            step();
            ++n;
        }
        return n;
    };

    aurora::Animator animator;
    animator.set_time_scale(1.0);  // 显式置基线：不依赖进入前 current() 槽位里已经是哪个实例
    const aurora::testing::ScopedCurrentAnimator scope{animator};  // 自驱路径的倍率取自 current()

    aurora::AnimationController via_animator{1.0};
    animator.drive(via_animator);
    aurora::State<double> sb{0.0};
    aurora::AnimatedValue<double> via_animated_value{sb, aurora::Tween<double>{0.0, 1.0}, 1.0};
    aurora::TweenAnimation<double> via_tween_animation{0.0};
    aurora::State<double> sd{0.0};
    aurora::TimelinePlayer player{aurora::TimelineSpec::sequence().add(0.2).add(0.3).build()};
    player.track<double>(0, aurora::Tween<double>{0.0, 10.0}, sd);

    via_animator.forward();
    via_animated_value.forward(0.0);
    via_tween_animation.animate_to(10.0, 1.0);
    player.forward(0.0);
    const int a_1x = frames_until([&]() -> bool { return via_animator.is_completed(); },
                                  [&]() -> void { animator.tick(tick_step); });
    const int b_1x = frames_until([&]() -> bool { return via_animated_value.is_completed(); },
                                  [&]() -> void { via_animated_value.tick(tick_step); });
    const int c_1x = frames_until([&]() -> bool { return !via_tween_animation.is_animating(); },
                                  [&]() -> void { via_tween_animation.tick(tick_step); });
    const int d_1x =
        frames_until([&]() -> bool { return player.is_completed(); }, [&]() -> void { player.tick(tick_step); });

    animator.set_time_scale(2.0);
    via_animator.reset();
    via_animator.forward();
    via_animated_value.controller().reset();
    via_animated_value.forward(0.0);
    via_tween_animation.animate_to(10.0, 1.0);
    player.forward(0.0);
    const int a_2x = frames_until([&]() -> bool { return via_animator.is_completed(); },
                                  [&]() -> void { animator.tick(tick_step); });
    const int b_2x = frames_until([&]() -> bool { return via_animated_value.is_completed(); },
                                  [&]() -> void { via_animated_value.tick(tick_step); });
    const int c_2x = frames_until([&]() -> bool { return !via_tween_animation.is_animating(); },
                                  [&]() -> void { via_tween_animation.tick(tick_step); });
    const int d_2x =
        frames_until([&]() -> bool { return player.is_completed(); }, [&]() -> void { player.tick(tick_step); });

    AURORA_TEST_CHECK_EQ(a_1x, 2 * a_2x);  // 出现 4 倍即漏缩放，出现 1/4 即双层缩放
    AURORA_TEST_CHECK_EQ(b_1x, 2 * b_2x);
    AURORA_TEST_CHECK_EQ(c_1x, 2 * c_2x);
    AURORA_TEST_CHECK_EQ(d_1x, 2 * d_2x);
    AURORA_TEST_CHECK_GT(a_2x, 0);
    AURORA_TEST_CHECK_GT(b_2x, 0);
    AURORA_TEST_CHECK_GT(c_2x, 0);
    AURORA_TEST_CHECK_GT(d_2x, 0);
    AURORA_TEST_CHECK_NEAR(via_animated_value.current(), 1.0, 1e-9);  // 缩放不改终值
    AURORA_TEST_CHECK_NEAR(via_tween_animation.get(), 10.0, 1e-9);
}

// @brief 判据 11(a)：冻结时登记控制器不置脏、绑定不写回，且 has_active 转假（帧循环不再被钉在满帧）。
AURORA_TEST_CASE(frozen_animator_neither_dirties_nor_writes_back) {
    aurora::State<double> s{0.0};
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    animator.bind(c, aurora::Tween<double>{0.0, 10.0}, s);

    int binding_calls = 0;
    int dirty_seen = 0;
    animator.add_binding([&c, &binding_calls, &dirty_seen]() -> void {
        ++binding_calls;
        if (c.dirty()) {
            ++dirty_seen;
        }
    });

    c.forward();
    animator.set_time_scale(0.0);
    for (int i = 0; i < 5; ++i) {
        animator.tick(0.2);
    }
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_FALSE(c.dirty());
    AURORA_TEST_CHECK_NEAR(s.get(), 0.0, 1e-12);
    AURORA_TEST_CHECK_EQ(binding_calls, 5);  // 回调仍逐帧执行（帧边界语义不变）
    AURORA_TEST_CHECK_EQ(dirty_seen, 0);  // 但一律不脏 → 不写回
    AURORA_TEST_CHECK_FALSE(animator.has_active());
}

// @brief 解冻重挂：`0 → >0` 恰置一位、由 `take_rearm_request` 一次性消费；冻结前进度不丢。
AURORA_TEST_CASE(thaw_arms_one_rearm_request_and_resumes_from_frozen_progress) {
    aurora::Animator animator;
    AURORA_TEST_CHECK_FALSE(animator.take_rearm_request());
    animator.set_time_scale(0.0);
    AURORA_TEST_CHECK_FALSE(animator.take_rearm_request());  // 进入冻结不置位——置位是「解冻」这一动作
    animator.set_time_scale(1.0);
    AURORA_TEST_CHECK_TRUE(animator.take_rearm_request());  // 0 → >0 置一位
    AURORA_TEST_CHECK_FALSE(animator.take_rearm_request());  // 消费即清（不做每帧全树扫描）

    aurora::State<double> s{0.0};
    aurora::AnimationController c{1.0};
    animator.bind(c, aurora::Tween<double>{0.0, 10.0}, s);
    c.forward();
    animator.tick(0.3);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.3, 1e-12);

    animator.set_time_scale(0.0);
    for (int i = 0; i < 4; ++i) {
        animator.tick(0.3);
    }
    AURORA_TEST_CHECK_NEAR(c.value(), 0.3, 1e-12);  // 冻结段进度停在原处

    animator.set_time_scale(1.0);
    animator.tick(0.35);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.65, 1e-12);  // 从冻结前的 0.3 续跑，而非从 0 重来
    int guard = 0;
    while (!c.is_completed() && guard < 10) {
        animator.tick(0.35);
        ++guard;
    }
    AURORA_TEST_CHECK_NEAR(s.get(), 10.0, 1e-9);  // 终值与不冻结时一致
}

// @brief 判据 12：无 current 实例时漏斗恒等返回（无头 / golden 基线逐比特不变）；倍率 1.0 时同样恒等。
AURORA_TEST_CASE(funnel_is_identity_without_current_animator) {
    aurora::Animator *saved = aurora::Animator::current();
    aurora::Animator animator;
    AURORA_TEST_CHECK_NEAR(animator.scale_frame_dt(2.5), 2.5, 1e-12);  // 默认倍率：恒等
    animator.set_time_scale(0.5);
    AURORA_TEST_CHECK_NEAR(animator.scale_frame_dt(2.5), 1.25, 1e-12);
    animator.set_time_scale(0.0);
    AURORA_TEST_CHECK_NEAR(animator.scale_frame_dt(2.5), 0.0, 1e-12);

    {
        const aurora::testing::ScopedCurrentAnimator scope{animator};
        animator.set_time_scale(0.25);
        AURORA_TEST_CHECK_NEAR(aurora::scaled_frame_dt(4.0), 1.0, 1e-12);  // 自由函数取 current() 的倍率
    }

    aurora::Animator::set_current(nullptr);  // 无运行实例（无头渲染 / golden / 手工驱动）
    AURORA_TEST_CHECK_NEAR(aurora::scaled_frame_dt(0.37), 0.37, 1e-12);
    AURORA_TEST_CHECK_NEAR(aurora::scaled_frame_dt(3.0), 3.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(aurora::scaled_frame_dt(-1.0), -1.0, 1e-12);  // 负值原样返回，由消费点的守卫视为不推进
    aurora::Animator::set_current(saved);
}

// @brief 判据 13：reduce_motion 与缩放正交且优先——短路路径不乘倍率。
AURORA_TEST_CASE(reduce_motion_short_circuit_ignores_time_scale) {
    const aurora::testing::ScopedReduceMotion reduce{true};
    aurora::Animator animator;
    animator.set_time_scale(0.01);  // 若短路吃了倍率，本帧只走 1% 进度而非到位
    aurora::AnimationController c{10.0};
    animator.drive(c);
    c.forward();
    animator.tick(0.1);
    AURORA_TEST_CHECK_TRUE(c.is_completed());
    AURORA_TEST_CHECK_NEAR(c.value(), 1.0, 1e-12);

    c.reset(0.5);
    c.reverse();
    animator.tick(0.1);
    AURORA_TEST_CHECK_NEAR(c.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(c.status() == aurora::AnimationStatus::Dismissed);
}

// ---- 关键帧分段曲线经 Animator 绑定 ----

// @brief 判据 31：`bind(controller, Keyframes, target)` 自动按各段起点曲线写回中间值。
AURORA_TEST_CASE(animator_keyframe_binding_applies_segment_curves) {
    aurora::State<double> s{0.0};
    aurora::AnimationController c{1.0};
    aurora::Animator animator;
    const aurora::Keyframes<double> kf{{
        {.time = 0.0, .value = 0.0, .curve = aurora::Curves::ease_in_quad()},
        {.time = 0.5, .value = 10.0},
        {.time = 1.0, .value = 20.0},
    }};
    animator.bind(c, kf, s);

    c.forward();
    animator.tick(0.25);  // 段 [0,0.5] 内 local 0.5 → 二次缓入 0.25 → 2.5（线性基线会是 5.0）
    AURORA_TEST_CHECK_NEAR(s.get(), 2.5, 1e-9);
    animator.tick(0.25);  // 段边界取该点值
    AURORA_TEST_CHECK_NEAR(s.get(), 10.0, 1e-9);
    animator.tick(0.25);  // 第二段缺省线性 → 中点 15.0
    AURORA_TEST_CHECK_NEAR(s.get(), 15.0, 1e-9);
    animator.tick(0.25);
    AURORA_TEST_CHECK_NEAR(s.get(), 20.0, 1e-9);
}

}  // namespace aurora::test_cases::utest_animator
