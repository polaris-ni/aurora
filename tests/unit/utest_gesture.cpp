/// 测试类型: unit
/// 目标单元: include/aurora/event/gesture.h
/// 测试说明: PinchRecognizer 激活条件/缩放比例追踪/抬指复位/锁定指针对抗第三指插入，RotationRecognizer 角度增量与符号、±180° 归一化与复位

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

}  // namespace aurora::test_cases::utest_gesture
