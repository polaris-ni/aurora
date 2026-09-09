/// 测试类型: unit
/// 目标单元: include/aurora/widget/skeleton.h
/// 测试说明: 覆盖 Skeleton——size_hint 与占满/回退布局语义、tick 驱动 shimmer 相位推进与回绕、
/// duration 覆写与非正容错、属性序列化往返与 describe 元数据

#include <chrono>
#include <string>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/skeleton.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_skeleton {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const Skeleton s;
    AURORA_TEST_CHECK_EQ(std::string{s.type_name()}, "Skeleton");
    const Size hint = s.size_hint();
    AURORA_TEST_CHECK_NEAR(hint.width, 0.0F, 1e-4F);  // 0 = 占满约束宽度
    AURORA_TEST_CHECK_NEAR(hint.height, 16.0F, 1e-4F);

    Json props;
    s.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["color"][0].get<int>(), 220);
    AURORA_TEST_CHECK_EQ(props["highlight"][0].get<int>(), 255);
    AURORA_TEST_CHECK_NEAR(props["duration"].get<double>(), 1.5, 1e-4);
    AURORA_TEST_CHECK_NEAR(props["height"].get<float>(), 16.0F, 1e-4F);
}

AURORA_TEST_CASE(zero_width_fills_constraint) {
    Skeleton s;
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 300.0F, 1e-4F);  // 宽 0 → 占满约束
    AURORA_TEST_CHECK_NEAR(s.size().height, 16.0F, 1e-4F);  // 默认高 16
}

AURORA_TEST_CASE(explicit_size_used_when_positive) {
    Skeleton s{Size{.width = 120.0F, .height = 20.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 20.0F, 1e-4F);

    // set_size 只更新占位尺寸本身、不标布局脏；AURORA_ENABLE_LAYOUT_CACHE 下约束不变时
    // Widget::layout 直接命中缓存返回旧尺寸，须经公开脏标记 mark_needs_layout() 才会真正重排。
    s.set_size(Size{.width = 50.0F, .height = 24.0F});
    s.mark_needs_layout();
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 24.0F, 1e-4F);
}

AURORA_TEST_CASE(zero_height_falls_back_to_16) {
    Skeleton s{Size{.width = 60.0F, .height = 0.0F}};
    LayoutEngine::layout(s, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(s.size().width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.size().height, 16.0F, 1e-4F);  // 高 0 → 回退 16
}

AURORA_TEST_CASE(tick_advances_phase_and_wraps) {
    Skeleton s{Size{.width = 120.0F, .height = 16.0F}};  // 显式尺寸构造才开启 gesture-tick 驱动
    // 起点须避开 epoch：tick_gestures 以 time_point{} 作为 start_ 的「未初始化」哨兵，
    // epoch 起点会让每次 tick 都命中哨兵分支重置 start_（相位恒滞后一拍）。
    // 固定偏移 1ms 的起点保持确定性，且不影响 elapsed = now - start_ 的差值计算。
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);

    s.tick(t0);  // 首帧登记起点，相位仍 0
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.0, 1e-4);

    s.tick(t0 + std::chrono::milliseconds(750));  // 0.75s / 1.5s = 0.5
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.5, 1e-4);

    s.tick(t0 + std::chrono::milliseconds(3000));  // 3.0s / 1.5s = 2 圈 → 回绕 0
    AURORA_TEST_CHECK_NEAR(s.phase(), 0.0, 1e-4);
    AURORA_TEST_CHECK_TRUE(s.phase() >= 0.0 && s.phase() < 1.0);
}

AURORA_TEST_CASE(set_duration_overrides_period) {
    // 同上：起点避开 epoch 哨兵（time_point{}），否则每次 tick 都会重置 start_ 导致相位滞后。
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1);

    Skeleton custom{Size{.width = 120.0F, .height = 16.0F}};
    custom.set_duration(0.75);
    custom.tick(t0);
    custom.tick(t0 + std::chrono::milliseconds(375));  // 0.375s / 0.75s = 0.5
    AURORA_TEST_CHECK_NEAR(custom.phase(), 0.5, 1e-4);

    Skeleton fallback{Size{.width = 120.0F, .height = 16.0F}};
    fallback.set_duration(-1.0);  // 非正 → 回落默认 1.5s
    fallback.tick(t0);
    fallback.tick(t0 + std::chrono::milliseconds(750));
    AURORA_TEST_CHECK_NEAR(fallback.phase(), 0.5, 1e-4);

    Json props;
    fallback.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["duration"].get<double>(), 1.5, 1e-4);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Skeleton src{Size{.width = 80.0F, .height = 22.0F}};
    src.set_color(Color(1, 2, 3, 4)).set_highlight(Color(5, 6, 7, 8)).set_duration(2.5);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["width"].get<float>(), 80.0F, 1e-4F);

    Skeleton dst;
    dst.deserialize_props(props);
    Json out;
    dst.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(out["width"].get<float>(), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out["height"].get<float>(), 22.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(out["color"][1].get<int>(), 2);
    AURORA_TEST_CHECK_EQ(out["highlight"][3].get<int>(), 8);
    AURORA_TEST_CHECK_NEAR(out["duration"].get<double>(), 2.5, 1e-4);

    // 反序列化后的占位尺寸直接驱动布局。
    LayoutEngine::layout(dst, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 22.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Skeleton::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Skeleton");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_duration = false;
    bool has_highlight = false;
    for (const auto& p : d.properties) {
        if (std::string{p.name} == "duration") {
            has_duration = true;
        }
        if (std::string{p.name} == "highlight") {
            has_highlight = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_duration);
    AURORA_TEST_CHECK_TRUE(has_highlight);
}

}  // namespace aurora::test_cases::utest_skeleton
