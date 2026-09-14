/// 测试类型: unit
/// 目标单元: include/aurora/event/gesture.h
/// 测试说明: PinchRecognizer 激活条件/缩放比例追踪/抬指复位/锁定指针对抗第三指插入，RotationRecognizer
/// 角度增量与符号、±180° 归一化与复位，DragRecognizer slop 起拖/主轴锁定/指针锁定对抗换指/
/// 鼠标与触摸双源/松手保持 delta 与 reset 清零，DragToDismiss 跟手 1:1 映射/裁决阈值两分支/
/// spring 回位不触发 dismissed/飞出触发/复位后二次拖动

#include <numbers>
#include <utility>
#include <vector>

#include "aurora/event/gesture.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_gesture {

namespace {

auto make_touch(std::vector<TouchPoint> points) -> TouchEvent {
    TouchEvent e;
    e.points = std::move(points);
    return e;
}

auto make_mouse(MouseAction action, Point pos) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.position = pos;
    return e;
}

}  // namespace

AURORA_TEST_CASE(pinch_starts_inactive_and_activates_on_two_fingers) {
    PinchRecognizer pinch;
    AURORA_TEST_CHECK_FALSE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 单指：不足以激活（等价 reset）
    pinch.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_FALSE(pinch.is_active());

    // 双指激活，初始 scale = 1
    pinch.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               TouchPoint{.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);
}

AURORA_TEST_CASE(pinch_scale_tracks_distance_ratio) {
    PinchRecognizer pinch;
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 拉开一倍 → scale 2
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 2.0F, 1e-5F);

    // 捏合一倍 → scale 0.5
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 5.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 0.5F, 1e-5F);
}

AURORA_TEST_CASE(pinch_resets_when_fingers_lift) {
    PinchRecognizer pinch;
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 2.0F, 1e-5F);

    // 抬起一指：识别器复位
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}, .is_active = false}}));
    AURORA_TEST_CHECK_FALSE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 新一轮手势从 1.0 重新起算
    pinch.on_touch(make_touch({{.id = 3, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 4, .position = Point{.x = 30.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);
}

AURORA_TEST_CASE(pinch_locks_pointer_pair_against_third_finger) {
    PinchRecognizer pinch;
    pinch.on_touch(make_touch({{.id = 5, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 7, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 第三、四指插入且排在遍历前列：仍锁定 id5/id7（距离 40），不回退到前两个活跃点
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}},
                               {.id = 5, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 7, .position = Point{.x = 40.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 4.0F, 1e-5F);
}

AURORA_TEST_CASE(rotation_delta_tracks_angle_with_sign) {
    RotationRecognizer rotation;
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(rotation.is_active());
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0F, 1e-5F);

    // 逆时针 90°（angle_delta 单位为弧度，+π/2）
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = 0.0F, .y = 10.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), std::numbers::pi_v<float> / 2, 1e-4F);

    // 转到 180°（+π）
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = -10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), std::numbers::pi_v<float>, 1e-4F);

    // 转至 -90°（向量 (0,-10)，相对初始 0° 的最短几何增量为顺时针 90°）→ 归一化 -π/2
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = 0.0F, .y = -10.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), -std::numbers::pi_v<float> / 2, 1e-4F);
}

AURORA_TEST_CASE(rotation_resets_and_normalizes_across_boundary) {
    RotationRecognizer rotation;
    // 初始角度 ≈ 180°
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = -10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0F, 1e-5F);

    // 微小顺时针回摆：-174.29° - 180° = -354.29° → 归一化 +5.71°（≈ 0.0996690 弧度）
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = -10.0F, .y = -1.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0996690F, 1e-5F);

    // 抬起复位：增量归零
    rotation.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                                  {.id = 2, .position = Point{.x = -10.0F, .y = -1.0F}, .is_active = false}}));
    AURORA_TEST_CHECK_FALSE(rotation.is_active());
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0F, 1e-6F);
}

// ---- DragRecognizer ----

AURORA_TEST_CASE(drag_slop_before_activation_and_axis_lock) {
    DragRecognizer drag;
    drag.slop = 8.0;

    // 未超 slop：不激活、轴 None。
    drag.on_mouse(make_mouse(MouseAction::Press, Point{.x = 100.0F, .y = 100.0F}));
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 104.0F, .y = 103.0F}));
    AURORA_TEST_CHECK_FALSE(drag.is_dragging());
    AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::None);

    // 超 slop（主水平）：激活 + 锁水平轴。
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 115.0F, .y = 104.0F}));
    AURORA_TEST_CHECK_TRUE(drag.is_dragging());
    AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::Horizontal);

    // 锁轴后斜向移动：delta 只含水平分量。
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 140.0F, .y = 160.0F}));
    const Point d = drag.delta();
    AURORA_TEST_CHECK_NEAR(d.x, 40.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.y, 0.0F, 1e-6F);

    // 松手：is_dragging 转 false，delta 保持供读取；reset 清零。
    drag.on_mouse(make_mouse(MouseAction::Release, Point{.x = 140.0F, .y = 160.0F}));
    AURORA_TEST_CHECK_FALSE(drag.is_dragging());
    AURORA_TEST_CHECK_NEAR(drag.delta().x, 40.0F, 1e-6F);
    drag.reset();
    AURORA_TEST_CHECK_NEAR(drag.delta().x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::None);
}

AURORA_TEST_CASE(drag_vertical_axis_lock) {
    DragRecognizer drag;
    drag.slop = 5.0;
    drag.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 2.0F, .y = 30.0F}));  // 主垂直
    AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::Vertical);
    const Point d = drag.delta();
    AURORA_TEST_CHECK_NEAR(d.x, 0.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.y, 30.0F, 1e-6F);
}

AURORA_TEST_CASE(drag_touch_source_locks_pointer_id) {
    DragRecognizer drag;
    drag.slop = 5.0;

    // 触摸按下指 1 → 起拖。
    drag.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}}));
    drag.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 20.0F, .y = 1.0F}}}));
    AURORA_TEST_CHECK_TRUE(drag.is_dragging());
    AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::Horizontal);

    // 第二指（id=2）插入移动：不劫持已锁定的指针 1，delta 不受污染。
    drag.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 25.0F, .y = 1.0F}},
                              TouchPoint{.id = 2, .position = Point{.x = 500.0F, .y = 500.0F}}}));
    AURORA_TEST_CHECK_NEAR(drag.delta().x, 25.0F, 1e-6F);

    // 锁定指针抬起（仅剩指 2）：结束本拖，不切换到指 2。
    drag.on_touch(make_touch({TouchPoint{.id = 1, .position = Point{.x = 25.0F, .y = 1.0F}, .is_active = false},
                              TouchPoint{.id = 2, .position = Point{.x = 600.0F, .y = 600.0F}}}));
    AURORA_TEST_CHECK_FALSE(drag.is_dragging());
    AURORA_TEST_CHECK_NEAR(drag.delta().x, 25.0F, 1e-6F);  // delta 保持
}

AURORA_TEST_CASE(drag_press_restarts_and_release_only_ends) {
    DragRecognizer drag;
    drag.slop = 5.0;
    // 第一次拖动结束后未 reset，新 Press 重开（清残留 delta/轴）。
    drag.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 50.0F, .y = 0.0F}));
    drag.on_mouse(make_mouse(MouseAction::Release, Point{.x = 50.0F, .y = 0.0F}));
    drag.on_mouse(make_mouse(MouseAction::Press, Point{.x = 200.0F, .y = 200.0F}));
    drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = 202.0F, .y = 203.0F}));
    AURORA_TEST_CHECK_FALSE(drag.is_dragging());  // 未超 slop（3.6 < 5）
    AURORA_TEST_CHECK_NEAR(drag.delta().x, 2.0F, 1e-6F);  // 相对新按点的位移
}

// ---- DragToDismiss ----

AURORA_TEST_CASE(dtd_follows_finger_one_to_one) {
    // 行程 100dp 水平：拖 40dp → progress 0.4（1:1 直接映射，非动画）。
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 40.0F, .y = 1.0F}));
    AURORA_TEST_CHECK_TRUE(dtd.is_dragging());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.4, 1e-6);

    // 超行程夹取 1；反方向夹取 0。
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 150.0F, .y = 1.0F}));
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 1.0, 1e-6);
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = -50.0F, .y = 1.0F}));
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.0, 1e-6);
}

AURORA_TEST_CASE(dtd_release_below_threshold_springs_back_silently) {
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed]() { ++dismissed; });

    // 拖到 0.3（< 阈值 0.5）松手：spring 回 0，不触发 dismissed。
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 30.0F, .y = 0.0F}));
    dtd.on_release();
    AURORA_TEST_CHECK_TRUE(dtd.is_animating());
    AURORA_TEST_CHECK_FALSE(dtd.is_dragging());  // 识别器已复位

    // 推进足够长：spring 收敛到 0，dismissed 不触发。
    for (int i = 0; i < 400 && dtd.is_animating(); ++i) {
        dtd.tick(1.0 / 60.0);
    }
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.0, 1e-3);
    AURORA_TEST_CHECK_EQ(dismissed, 0);
}

AURORA_TEST_CASE(dtd_release_above_threshold_flies_out_and_fires_dismissed) {
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed]() { ++dismissed; });

    // 拖到 0.7（≥ 阈值）松手：spring 飞 1，触发 dismissed。
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 0.0F}));
    dtd.on_release();
    for (int i = 0; i < 400 && dtd.is_animating(); ++i) {
        dtd.tick(1.0 / 60.0);
    }
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 1.0, 1e-3);
    AURORA_TEST_CHECK_EQ(dismissed, 1);

    // 飞出后可再次拖动（progress 从当前值重启跟手）。
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 25.0F, .y = 0.0F}));
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.25, 1e-6);
}

AURORA_TEST_CASE(dtd_velocity_estimated_along_axis_into_spring) {
    // 快速甩动（两帧 60dp）后松手在低进度：速度方向正 → 初速度进 spring。
    // 通过构造欠阻尼 spring 观察超调验证速度被采纳（无速度时 0.3→0 单调不超调）。
    SpringDescription bouncy{.stiffness = 100.0, .damping = 8.0, .mass = 1.0};  // 欠阻尼
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, bouncy);
    dtd.recognizer_slop_for_test(5.0);
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 30.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 90.0F, .y = 0.0F}));  // 高速 → 速度 3600/s
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.9, 1e-6);

    // 松手（0.9 ≥ 0.5 → 飞出）：初速同向，spring 起步应越过 1 前的瞬时值更快逼近。
    dtd.on_release();
    dtd.tick(1.0 / 60.0);
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 1.0, 0.5);  // 已接近/到达端点（夹取 ≤1）
}

}  // namespace aurora::test_cases::utest_gesture
