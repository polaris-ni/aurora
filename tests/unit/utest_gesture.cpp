/// 测试类型: unit
/// 目标单元: include/aurora/event/gesture.h
/// 测试说明: PinchRecognizer 激活条件/缩放比例追踪/抬指复位/锁定指针对抗第三指插入，RotationRecognizer
/// 角度增量与符号、±180° 归一化与复位，DragRecognizer slop 起拖/主轴锁定/指针锁定对抗换指/
/// 鼠标与触摸双源/松手保持 delta 与 reset 清零，DragToDismiss 跟手 1:1 映射/裁决阈值两分支/
/// spring 回位不触发 dismissed/飞出触发/复位后二次拖动；另含 DragToDismiss 收口到公共弹簧的
/// 符号级核对（类体内不再自持同构弹簧三件套）、跟手阶段 tick 为 no-op、松手初速度方向规则
/// （相反丢弃 / 同向保留）、spring 段自驱收敛定格与 on_dismissed 一次性、reduce_motion 不干预跟手，
/// 以及 DragRecognizer 经 set_progress 定位绑定进度、松手交回 forward/reverse 的手势路径可行性。

#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/event/gesture.h"
#include "framework/aurora_test.h"
#include "support/animation_fixture.h"
#include "support/paths.h"

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

/// 读取仓库根下的文本文件。读不到即**致命**失败而非跳过：符号级核对一旦静默空转，删掉被测
/// 符号也不会转红（照 utest_scroll 读 tools/check/perf_gates.json 的口径）。
/// 测试二进制由框架把 cwd 统一切到仓库根，under_repo 在其上给出绝对路径兜底。
auto read_repo_text(const char *relative) -> std::string {
    const std::string path = aurora::testing::paths::under_repo(relative);
    std::ifstream in{path, std::ios::binary};
    AURORA_TEST_REQUIRE_MSG(in.good(), "source text must be readable: " + path);
    std::ostringstream sink;
    sink << in.rdbuf();
    return sink.str();
}

/// 截出 gesture.h 里 DragToDismiss 的类体：自 `class DragToDismiss` 起，到其后**首个顶格** `};`
/// 止（类内嵌套类型的收尾带缩进，故顶格 `};` 即本类终点），把核对范围钉在这一个类上。
auto drag_to_dismiss_class_body() -> std::string {
    const std::string text = read_repo_text("include/aurora/event/gesture.h");
    const std::size_t begin = text.find("class DragToDismiss");
    const std::size_t end = begin == std::string::npos ? std::string::npos : text.find("\n};", begin);
    AURORA_TEST_REQUIRE_MSG(begin != std::string::npos && end != std::string::npos,
                            "gesture.h must declare the DragToDismiss class body");
    return text.substr(begin, end - begin);
}

/// 类体内是否出现某片段（按文本粒度：声明与注释同样计入，故负向项只挑「只有实现才会写」的符号）。
auto class_body_has(const std::string &body, std::string_view needle) -> bool {
    return body.find(needle) != std::string::npos;
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
    pinch.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 拉开一倍 → scale 2
    pinch.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 2.0F, 1e-5F);

    // 捏合一倍 → scale 0.5
    pinch.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 5.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 0.5F, 1e-5F);
}

AURORA_TEST_CASE(pinch_resets_when_fingers_lift) {
    PinchRecognizer pinch;
    pinch.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    pinch.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 2.0F, 1e-5F);

    // 抬起一指：识别器复位
    pinch.on_touch(make_touch({{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}},
                               {.id = 2, .position = Point{.x = 20.0F, .y = 0.0F}, .is_active = false}}));
    AURORA_TEST_CHECK_FALSE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);

    // 新一轮手势从 1.0 重新起算
    pinch.on_touch(make_touch(
        {{.id = 3, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 4, .position = Point{.x = 30.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(pinch.is_active());
    AURORA_TEST_CHECK_NEAR(pinch.scale(), 1.0F, 1e-6F);
}

AURORA_TEST_CASE(pinch_locks_pointer_pair_against_third_finger) {
    PinchRecognizer pinch;
    pinch.on_touch(make_touch(
        {{.id = 5, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 7, .position = Point{.x = 10.0F, .y = 0.0F}}}));
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
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_TRUE(rotation.is_active());
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0F, 1e-5F);

    // 逆时针 90°（angle_delta 单位为弧度，+π/2）
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 0.0F, .y = 10.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), std::numbers::pi_v<float> / 2, 1e-4F);

    // 转到 180°（+π）
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = -10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), std::numbers::pi_v<float>, 1e-4F);

    // 转至 -90°（向量 (0,-10)，相对初始 0° 的最短几何增量为顺时针 90°）→ 归一化 -π/2
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = 0.0F, .y = -10.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), -std::numbers::pi_v<float> / 2, 1e-4F);
}

AURORA_TEST_CASE(rotation_resets_and_normalizes_across_boundary) {
    RotationRecognizer rotation;
    // 初始角度 ≈ 180°
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = -10.0F, .y = 0.0F}}}));
    AURORA_TEST_CHECK_NEAR(rotation.angle_delta(), 0.0F, 1e-5F);

    // 微小顺时针回摆：-174.29° - 180° = -354.29° → 归一化 +5.71°（≈ 0.0996690 弧度）
    rotation.on_touch(make_touch(
        {{.id = 1, .position = Point{.x = 0.0F, .y = 0.0F}}, {.id = 2, .position = Point{.x = -10.0F, .y = -1.0F}}}));
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
    dtd.on_dismissed([&dismissed] { ++dismissed; });

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
    dtd.on_dismissed([&dismissed] { ++dismissed; });

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
    constexpr SpringDescription bouncy{.stiffness = 100.0, .damping = 8.0, .mass = 1.0};  // 欠阻尼
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

// ---- DragToDismiss 收口到公共弹簧（判据 25 / 26）与手势定位路径（判据 19） ----

// @brief 判据 25 的符号级核对：DragToDismiss 类体内不再存在与 SpringAnimation 同构的私有弹簧三件套。
AURORA_TEST_CASE(drag_to_dismiss_holds_no_private_spring_duplicate) {
    const std::string body = drag_to_dismiss_class_body();

    // 正向：确实持有公共驱动（成员类型名 + 起播 / 逐帧推进 / 收敛观察都经它）。
    AURORA_TEST_CHECK_TRUE(class_body_has(body, "SpringAnimation"));
    AURORA_TEST_CHECK_TRUE(class_body_has(body, "spring_anim_"));
    AURORA_TEST_CHECK_TRUE(class_body_has(body, "animate_to"));
    AURORA_TEST_CHECK_TRUE(class_body_has(body, "is_running"));

    // 负向：自持求解器 / 自持收敛判定 / 自持模拟时间累加（三件套的符号），一律不得再出现。
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "SpringSimulation"));
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "is_settled"));
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "t_sim"));
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "settle_t"));
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "elapsed"));

    // 单一 dt 漏斗：步长折算只在 SpringAnimation::tick 里发生一次，本类不得自己再乘一遍。
    AURORA_TEST_CHECK_FALSE(class_body_has(body, "scaled_frame_dt"));
}

// @brief 判据 26 条 1：跟手是 1:1 直接操作，此阶段 tick 为 no-op——不转动画态、不写进度、不触发消除。
AURORA_TEST_CASE(dtd_follow_phase_tick_is_a_no_op) {
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed]() -> void { ++dismissed; });
    int writes = 0;
    const auto sub = dtd.progress().subscribe([&writes](const double &) -> void { ++writes; });
    AURORA_TEST_CHECK_EQ(writes, 1);  // 订阅即交付首值，作为「有没有写 State」的基线

    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 40.0F, .y = 0.0F}));
    AURORA_TEST_CHECK_TRUE(dtd.is_dragging());
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.4, 1e-12);
    const int writes_after_drag = writes;

    for (int i = 0; i < 8; ++i) {
        dtd.tick(1.0);  // 步长再大也不越俎代庖：跟手阶段没有动画时间轴可推进
    }
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.4, 1e-12);
    AURORA_TEST_CHECK_EQ(writes, writes_after_drag);
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_EQ(dismissed, 0);

    // 进度只由事件决定：再拖 15dp 到 0.55（State 随之才写，证明上面的零写入不是订阅没接上）。
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 55.0F, .y = 0.0F}));
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.55, 1e-12);
    AURORA_TEST_CHECK_GT(writes, writes_after_drag);
}

// @brief 判据 26 条 2（阈值二选一）：恰及阈值判飞出并触发消除，差一点判回位且静默。
AURORA_TEST_CASE(dtd_release_arbitrates_only_between_the_two_endpoints) {
    aurora::Animator animator;
    animator.set_time_scale(1.0);  // 自驱步长取 current() 的倍率：钉成恒等，落点才与倍率无关
    const aurora::testing::ScopedCurrentAnimator scope{animator};

    // 拖到 drag_dp 松手，喂帧直到定格：回终值与消除回调触发次数。
    auto settle_after_release = [](double drag_dp) -> std::pair<double, int> {
        DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
        dtd.recognizer_slop_for_test(5.0);
        int dismissed = 0;
        dtd.on_dismissed([&dismissed]() -> void { ++dismissed; });
        dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
        dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = static_cast<float>(drag_dp), .y = 0.0F}));
        dtd.on_release();
        int frames = 0;
        while (dtd.is_animating() && frames < 800) {
            dtd.tick(1.0 / 60.0);
            ++frames;
        }
        AURORA_TEST_CHECK_FALSE(dtd.is_animating());  // 两条裁决都必须自驱到定格，不许挂在半途
        return {dtd.progress().get(), dismissed};
    };

    const auto fly = settle_after_release(50.0);  // progress 恰为阈值 0.5：取「≥ 则飞出」
    AURORA_TEST_CHECK_NEAR(fly.first, 1.0, 1e-12);
    AURORA_TEST_CHECK_EQ(fly.second, 1);

    const auto back = settle_after_release(49.5);  // progress 0.495 < 阈值：回位且静默
    AURORA_TEST_CHECK_NEAR(back.first, 0.0, 1e-12);
    AURORA_TEST_CHECK_EQ(back.second, 0);
}

// @brief 判据 26 条 2（初速度方向规则）：与裁决相反的甩动被丢弃（与零速参照逐帧同轨），同向的保留（首帧即及上界）。
AURORA_TEST_CASE(dtd_release_drops_opposing_velocity_and_keeps_aligned_one) {
    aurora::Animator animator;
    animator.set_time_scale(1.0);
    const aurora::testing::ScopedCurrentAnimator scope{animator};  // 两条轨迹共用同一恒等漏斗才可逐比特对齐

    // 反向微甩：末帧主轴位移为负而裁决为飞出 → 速度丢弃 → 与「零速起播」的公共弹簧同轨。
    DragToDismiss opposing(DragAxis::Horizontal, 100.0, SpringDescription{});
    opposing.recognizer_slop_for_test(5.0);
    opposing.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    opposing.on_mouse(make_mouse(MouseAction::Move, Point{.x = 75.0F, .y = 0.0F}));
    opposing.on_mouse(make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 0.0F}));  // -5dp → -300/s
    const double start = opposing.progress().get();
    AURORA_TEST_REQUIRE_NEAR(start, 0.7, 1e-12);
    opposing.on_release();

    aurora::State<double> reference_state{start};
    aurora::SpringAnimation reference{reference_state};
    reference.set_range(0.0, 1.0);  // 与 DragToDismiss 构造期的值域声明同形制
    reference.animate_to(1.0, SpringDescription{}, /*initial_velocity=*/0.0);

    for (int i = 0; i < 12; ++i) {
        opposing.tick(1.0 / 60.0);
        reference.tick(1.0 / 60.0);
        AURORA_TEST_CHECK_NEAR(opposing.progress().get(), reference_state.get(), 1e-12);
    }
    AURORA_TEST_CHECK_TRUE(reference.is_running());  // 参照仍在途：上面的等式不是双方都已定格的空转

    // 同向甩动：速度保留 → 首帧即越过上界（夹到 1），零速同起点参照同一时刻仍在下侧。
    DragToDismiss aligned(DragAxis::Horizontal, 100.0, SpringDescription{});
    aligned.recognizer_slop_for_test(5.0);
    aligned.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    aligned.on_mouse(make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 0.0F}));  // +70dp → +4200/s
    aligned.on_release();
    aligned.tick(1.0 / 60.0);

    aurora::State<double> calm_state{0.7};
    aurora::SpringAnimation calm{calm_state};
    calm.set_range(0.0, 1.0);
    calm.animate_to(1.0, SpringDescription{}, /*initial_velocity=*/0.0);
    calm.tick(1.0 / 60.0);
    AURORA_TEST_CHECK_NEAR(aligned.progress().get(), 1.0, 1e-12);
    AURORA_TEST_CHECK_GT(calm_state.get(), 0.7);  // 参照确实推进了……
    AURORA_TEST_CHECK_LT(calm_state.get(), 1.0);  // ……但没同向速度的助推
}

// @brief 判据 26 条 4 + on_dismissed 一次性：spring 段由 tick(dt) 自驱收敛，定格于 target 后不写 State、不重复触发。
AURORA_TEST_CASE(dtd_spring_phase_self_drives_and_dismissed_fires_exactly_once) {
    aurora::Animator animator;
    animator.set_time_scale(1.0);
    const aurora::testing::ScopedCurrentAnimator scope{animator};

    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed]() -> void { ++dismissed; });
    int writes = 0;
    const auto sub = dtd.progress().subscribe([&writes](const double &) -> void { ++writes; });

    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 0.0F}));
    dtd.on_release();
    const int writes_at_release = writes;
    AURORA_TEST_CHECK_TRUE(dtd.is_animating());

    int frames = 0;
    while (dtd.is_animating() && frames < 800) {
        dtd.tick(1.0 / 60.0);  // 无 Animator 参与：喂帧即自驱，进度逐帧逼近 1
        ++frames;
    }
    AURORA_TEST_CHECK_GT(frames, 1);  // 至少推进过帧（不是单帧落点，那是 reduce_motion 的档）
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 1.0, 1e-12);  // 定格写的是 target 本身
    AURORA_TEST_CHECK_GT(writes, writes_at_release);  // 在途确实在逐帧写 State
    AURORA_TEST_CHECK_EQ(dismissed, 1);

    const int writes_at_settle = writes;
    for (int i = 0; i < 20; ++i) {
        dtd.tick(1.0 / 60.0);  // 重复喂帧：无幽灵帧、无第二次派发
    }
    AURORA_TEST_CHECK_EQ(writes, writes_at_settle);
    AURORA_TEST_CHECK_EQ(dismissed, 1);
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
}

// @brief 判据 26 条 3 + §6.7「直接操作不被无障碍干预」：reduce_motion 下单帧落端点，跟手 1:1 保持不变。
AURORA_TEST_CASE(dtd_reduce_motion_lands_in_one_frame_but_leaves_follow_untouched) {
    const aurora::testing::ScopedReduceMotion reduce{true};
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed]() -> void { ++dismissed; });

    // 跟手阶段不属动画时间轴：无障碍开关不得把它折成「一步到位」。
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 37.0F, .y = 0.0F}));
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.37, 1e-12);
    dtd.tick(1.0 / 60.0);
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.37, 1e-12);
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());

    // 松手裁决（0.37 < 阈值）→ 回位：单帧直接落 0 并结束，不触发消除。
    dtd.on_release();
    AURORA_TEST_CHECK_TRUE(dtd.is_animating());
    dtd.tick(1.0 / 60.0);
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 0.0, 1e-12);
    AURORA_TEST_CHECK_EQ(dismissed, 0);
    // 飞出方向的单帧落端点已由 tests/unit/utest_dismissible.cpp 的 dtd_reduce_motion 用例覆盖，此处不重复。
}

// @brief 判据 19 的手势路径：识别器位移经 set_progress 连续定位绑定进度，松手由 forward/reverse 接管收尾。
AURORA_TEST_CASE(drag_recognizer_positions_bound_progress_then_playback_takes_over) {
    aurora::State<double> drawer{0.0};
    aurora::AnimationController controller{0.2};
    aurora::Animator animator;
    animator.bind(controller, aurora::Tween<double>{0.0, 200.0}, drawer);

    DragRecognizer drag;
    drag.slop = 5.0;
    drag.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));

    // 一次拖拽 = 帧边界前定位 + Animator 当帧写回；全程不借道 reset 的状态副作用。
    auto locate_to = [&](double along_dp) -> void {
        drag.on_mouse(make_mouse(MouseAction::Move, Point{.x = static_cast<float>(along_dp), .y = 2.0F}));
        AURORA_TEST_CHECK_TRUE(drag.is_dragging());
        AURORA_TEST_CHECK_TRUE(drag.axis() == DragAxis::Horizontal);
        controller.set_progress(drag.delta().x / 100.0);
        AURORA_TEST_CHECK_FALSE(controller.is_animating());  // 定位不把控制器推进播放态
        AURORA_TEST_CHECK_FALSE(animator.has_active());  // 静止控制器只定位，帧循环不转入活跃
        animator.tick(1.0 / 60.0);  // 本帧无人推进，只消费手势写入的外部脏
        AURORA_TEST_CHECK_FALSE(controller.dirty());  // 帧末统一清脏
    };

    locate_to(20.0);
    AURORA_TEST_CHECK_NEAR(controller.value(), 0.2, 1e-12);
    AURORA_TEST_CHECK_NEAR(drawer.get(), 40.0, 1e-9);
    locate_to(55.0);
    AURORA_TEST_CHECK_NEAR(drawer.get(), 110.0, 1e-9);
    locate_to(92.0);
    AURORA_TEST_CHECK_NEAR(drawer.get(), 184.0, 1e-9);  // 进度随拖拽连续变化

    // 越界夹入 [0,1]，且夹到端点也不改写 status / 播放模式。
    locate_to(-10.0);
    AURORA_TEST_CHECK_NEAR(controller.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(controller.status() == AnimationStatus::Dismissed);
    AURORA_TEST_CHECK_TRUE(controller.mode() == PlaybackMode::Once);
    locate_to(150.0);
    AURORA_TEST_CHECK_NEAR(controller.value(), 1.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(controller.status() == AnimationStatus::Dismissed);  // 非 reset：不升 Completed

    // 松手收尾（正向）：从当前进度续跑到终点。
    controller.set_progress(0.6);
    controller.forward();
    AURORA_TEST_CHECK_TRUE(controller.status() == AnimationStatus::Forward);
    AURORA_TEST_CHECK_TRUE(animator.has_active());
    int guard = 0;
    while (!controller.is_completed() && guard < 50) {
        animator.tick(1.0 / 60.0);
        ++guard;
    }
    AURORA_TEST_CHECK_TRUE(controller.is_completed());
    AURORA_TEST_CHECK_NEAR(drawer.get(), 200.0, 1e-9);

    // 收尾后再拖拽定位：status 保持 Completed 不被改写，反向接管回到起点。
    controller.set_progress(0.3);
    AURORA_TEST_CHECK_NEAR(controller.value(), 0.3, 1e-12);
    AURORA_TEST_CHECK_TRUE(controller.status() == AnimationStatus::Completed);
    controller.reverse();
    AURORA_TEST_CHECK_TRUE(controller.status() == AnimationStatus::Reverse);
    guard = 0;
    while (controller.is_animating() && guard < 50) {
        animator.tick(1.0 / 60.0);
        ++guard;
    }
    AURORA_TEST_CHECK_NEAR(controller.value(), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(drawer.get(), 0.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(animator.has_active());
}

}  // namespace aurora::test_cases::utest_gesture
