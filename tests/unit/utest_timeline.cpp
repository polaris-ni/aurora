/// 测试类型: unit
/// 目标单元: include/aurora/animation/timeline.h
/// 测试说明: 覆盖 lerp 五类几何/颜色重载与算术截断、Tween 端点/夹取/曲线塑形与访问器、Keyframes
/// 停靠点排序/区间外夹取/空表缺省/同时刻停靠点边界

#include "aurora/animation/timeline.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_timeline {

/// @brief lerp 覆盖算术类型（含整型截断）与 Point/Size/EdgeInsets/Rect 复合重载。
AURORA_TEST_CASE(lerp_arithmetic_and_geometry_overloads) {
    AURORA_TEST_CHECK_NEAR(aurora::lerp(0.0, 10.0, 0.25), 2.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(aurora::lerp(2.0F, 8.0F, 0.5F), 5.0F, 1e-6F);
    // 整型走通用模板：结果向零截断。
    AURORA_TEST_CHECK_EQ(aurora::lerp(0, 10, 0.25), 2);
    AURORA_TEST_CHECK_EQ(aurora::lerp(0, 10, 0.0), 0);
    AURORA_TEST_CHECK_EQ(aurora::lerp(0, 10, 1.0), 10);

    const auto p = aurora::lerp(aurora::Point{.x = 2.0F, .y = 4.0F}, aurora::Point{.x = 6.0F, .y = 12.0F}, 0.25);
    AURORA_TEST_CHECK_NEAR(p.x, 3.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(p.y, 6.0F, 1e-6F);

    const auto s = aurora::lerp(aurora::Size{.width = 10.0F, .height = 20.0F},
                                aurora::Size{.width = 30.0F, .height = 40.0F}, 0.25);
    AURORA_TEST_CHECK_NEAR(s.width, 15.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(s.height, 25.0F, 1e-6F);

    const auto e = aurora::lerp(aurora::EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 8.0F, .bottom = 8.0F},
                                aurora::EdgeInsets{.left = 8.0F, .top = 8.0F, .right = 0.0F, .bottom = 0.0F}, 0.5);
    AURORA_TEST_CHECK_NEAR(e.left, 4.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(e.top, 4.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(e.right, 4.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(e.bottom, 4.0F, 1e-6F);

    const auto r = aurora::lerp(aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                                             .size = aurora::Size{.width = 10.0F, .height = 10.0F}},
                                aurora::Rect{.origin = aurora::Point{.x = 20.0F, .y = 30.0F},
                                             .size = aurora::Size{.width = 30.0F, .height = 50.0F}},
                                0.5);
    AURORA_TEST_CHECK_NEAR(r.origin.x, 10.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(r.origin.y, 15.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(r.size.width, 20.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(r.size.height, 30.0F, 1e-6F);
}

/// @brief Color 插值逐通道四舍五入（lround：半值远离零），端点处逐通道精确。
AURORA_TEST_CASE(lerp_color_rounds_each_channel) {
    const auto mid = aurora::lerp(aurora::Color::transparent(), aurora::Color::white(), 0.5);
    AURORA_TEST_CHECK_EQ(mid.r, 128);  // lround(127.5) = 128
    AURORA_TEST_CHECK_EQ(mid.g, 128);
    AURORA_TEST_CHECK_EQ(mid.b, 128);
    AURORA_TEST_CHECK_EQ(mid.a, 128);

    const auto quarter = aurora::lerp(aurora::Color::transparent(), aurora::Color::white(), 0.25);
    AURORA_TEST_CHECK_EQ(quarter.r, 64);  // lround(63.75) = 64

    const auto at_begin = aurora::lerp(aurora::Color::blue(), aurora::Color::red(), 0.0);
    AURORA_TEST_CHECK(at_begin == aurora::Color::blue());
    const auto at_end = aurora::lerp(aurora::Color::blue(), aurora::Color::red(), 1.0);
    AURORA_TEST_CHECK(at_end == aurora::Color::red());
}

/// @brief Tween 默认线性：端点精确、区间内线性、区间外被曲线夹取回端点。
AURORA_TEST_CASE(tween_endpoints_clamp_and_midpoint) {
    const aurora::Tween<double> tween{2.0, 12.0};
    AURORA_TEST_CHECK_NEAR(tween.value(0.0), 2.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tween.value(0.5), 7.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tween.value(1.0), 12.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tween.value(-0.25), 2.0, 1e-12);  // t<0 夹取到 begin
    AURORA_TEST_CHECK_NEAR(tween.value(1.75), 12.0, 1e-12);  // t>1 夹取到 end

    const aurora::Tween<int> ints{0, 10};
    AURORA_TEST_CHECK_EQ(ints.value(0.5), 5);  // 整型补间同样截断
}

/// @brief Tween 按曲线塑形进度：value = lerp(begin, end, curve.transform(t))。
AURORA_TEST_CASE(tween_applies_curve_shape) {
    const aurora::Tween<double> shaped{0.0, 100.0, aurora::Curves::ease_in_quad()};
    AURORA_TEST_CHECK_NEAR(shaped.value(0.5), 25.0, 1e-9);  // ease_in_quad(0.5)=0.25
    AURORA_TEST_CHECK_NEAR(shaped.value(0.0), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(shaped.value(1.0), 100.0, 1e-9);

    const aurora::Tween<double> custom{0.0, 100.0, aurora::Curve{[](double t) -> double { return t * t; }}};
    AURORA_TEST_CHECK_NEAR(custom.value(0.5), 25.0, 1e-9);
}

/// @brief Tween 访问器返回构造时的配置，setter 修改后立即生效。
AURORA_TEST_CASE(tween_accessors_and_setters) {
    aurora::Tween<double> tween{1.0, 9.0, aurora::Curves::ease_in()};
    AURORA_TEST_CHECK_NEAR(tween.begin(), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tween.end(), 9.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(tween.curve().kind() == aurora::CurveKind::EaseIn);

    tween.set_begin(0.0);
    tween.set_end(10.0);
    tween.set_curve(aurora::Curves::linear());
    AURORA_TEST_CHECK_NEAR(tween.begin(), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tween.end(), 10.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(tween.curve().kind() == aurora::CurveKind::Linear);
    AURORA_TEST_CHECK_NEAR(tween.value(0.5), 5.0, 1e-12);
}

/// @brief Keyframes 构造时按时间排序停靠点，区间内线性插值。
AURORA_TEST_CASE(keyframes_sort_stops_and_interpolate) {
    const aurora::Keyframes<double> kf{
        {{.time = 0.5, .value = 10.0}, {.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 30.0}}};
    AURORA_TEST_CHECK_EQ(kf.stops().size(), static_cast<std::size_t>(3));
    AURORA_TEST_CHECK_NEAR(kf.stops()[0].time, 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(kf.stops()[1].time, 0.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(kf.stops()[2].time, 1.0, 1e-12);

    AURORA_TEST_CHECK_NEAR(kf.value(0.25), 5.0, 1e-12);  // 前 half 线性
    AURORA_TEST_CHECK_NEAR(kf.value(0.75), 20.0, 1e-12);  // 后 half 线性
    AURORA_TEST_CHECK_NEAR(kf.value(0.0), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(kf.value(1.0), 30.0, 1e-12);
}

/// @brief Keyframes 边界路径：空表返回 T{}、区间外夹取端点、同时刻停靠点取较早者。
AURORA_TEST_CASE(keyframes_empty_clamp_and_duplicate_time_edges) {
    const aurora::Keyframes<double> empty{};
    AURORA_TEST_CHECK_NEAR(empty.value(0.3), 0.0, 1e-12);  // 空表 → T{}
    AURORA_TEST_CHECK_NEAR(empty.value(-1.0), 0.0, 1e-12);

    const aurora::Keyframes<double> kf{{{.time = 0.2, .value = 4.0}, {.time = 0.8, .value = 8.0}}};
    AURORA_TEST_CHECK_NEAR(kf.value(-0.5), 4.0, 1e-12);  // 低于首停靠点 → 首值
    AURORA_TEST_CHECK_NEAR(kf.value(1.5), 8.0, 1e-12);  // 高于末停靠点 → 末值

    // 同时刻停靠点（0.5 重复）：值一致时无论排序落位如何结果确定（std::sort 非稳定，
    // 同时刻异值停靠点的取舍未指定，见返回报告）。
    const aurora::Keyframes<double> dup{{{.time = 0.0, .value = 1.0},
                                         {.time = 0.5, .value = 5.0},
                                         {.time = 0.5, .value = 5.0},
                                         {.time = 1.0, .value = 2.0}}};
    AURORA_TEST_CHECK_NEAR(dup.value(0.5), 5.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(dup.value(0.75), 3.5, 1e-12);  // 与后段 (0.5,5)→(1.0,2) 插值
}

}  // namespace aurora::test_cases::utest_timeline
