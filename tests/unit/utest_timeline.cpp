/// 测试类型: unit
/// 目标单元: include/aurora/animation/timeline.h
/// 测试说明: 覆盖 lerp 五类几何/颜色重载与算术截断、Tween 端点/夹取/曲线塑形与访问器、Keyframes
/// 停靠点排序/区间外夹取/空表缺省/同时刻停靠点边界、TimelineSpec 区间树计算（sequence 游标
/// 推进 / parallel 取 max / staggered 偏移公式 / 嵌套组平移）、TimelineInterval::local 区间外
/// 夹取、槽位深度优先序、空 spec / 非法时长夹取、duration 汇总

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

/// @brief TimelineInterval::local 的区间内映射与区间外夹取端点语义。
AURORA_TEST_CASE(timeline_interval_local_clamps_outside) {
    const aurora::TimelineInterval iv{.begin = 0.25, .end = 0.75};
    AURORA_TEST_CHECK_NEAR(iv.local(0.0), 0.0, 1e-12);    // t ≤ begin → 0（未开始）
    AURORA_TEST_CHECK_NEAR(iv.local(0.25), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(iv.local(0.5), 0.5, 1e-12);    // 区间中点 → 局部中点
    AURORA_TEST_CHECK_NEAR(iv.local(0.75), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(iv.local(1.0), 1.0, 1e-12);    // t ≥ end → 1（已完成）
    AURORA_TEST_CHECK_TRUE(iv.contains(0.5));
    AURORA_TEST_CHECK_FALSE(iv.contains(0.1));

    // 零宽区间（夹取后的退化段）：任何 t ≥ begin 都取 1，避免除零。
    const aurora::TimelineInterval zero{.begin = 0.4, .end = 0.4};
    AURORA_TEST_CHECK_NEAR(zero.local(0.3), 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(zero.local(0.4), 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(zero.local(0.9), 1.0, 1e-12);
}

/// @brief sequence：游标推进、槽位深度优先序、duration 汇总。
AURORA_TEST_CASE(timeline_sequence_cursor_and_dfs_slots) {
    const auto tl = aurora::TimelineSpec::sequence().add(0.2).add(0.3).add(0.1).build();
    AURORA_TEST_CHECK_EQ(tl.slot_count(), std::size_t{3});
    AURORA_TEST_CHECK_NEAR(tl.duration(), 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(0).begin, 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(0).end, 0.2 / 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(1).begin, 0.2 / 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(1).end, 0.5 / 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(2).begin, 0.5 / 0.6, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(2).end, 1.0, 1e-12);
    // 越界防御：返回全区间。
    AURORA_TEST_CHECK_NEAR(tl.interval(99).begin, 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(99).end, 1.0, 1e-12);
}

/// @brief parallel：子段同起点、组长 = max(子)，组尾允许间隙。
AURORA_TEST_CASE(timeline_parallel_anchor_and_gap) {
    const auto tl = aurora::TimelineSpec::parallel().add(0.3).add(0.5).add(0.2).build();
    AURORA_TEST_CHECK_EQ(tl.slot_count(), std::size_t{3});
    AURORA_TEST_CHECK_NEAR(tl.duration(), 0.5, 1e-12);
    for (std::size_t i = 0; i < 3; ++i) {
        AURORA_TEST_CHECK_NEAR(tl.interval(i).begin, 0.0, 1e-12);  // 全部锚定组起点
    }
    AURORA_TEST_CHECK_NEAR(tl.interval(0).end, 0.3 / 0.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(1).end, 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(2).end, 0.2 / 0.5, 1e-12);
}

/// @brief staggered：偏移公式 [i*(item+gap), +item]；gap=0 退化为 sequence。
AURORA_TEST_CASE(timeline_staggered_offsets) {
    const auto tl = aurora::TimelineSpec::staggered(0.2, 0.1, 3).build();
    AURORA_TEST_CHECK_EQ(tl.slot_count(), std::size_t{3});
    AURORA_TEST_CHECK_NEAR(tl.duration(), 2 * (0.2 + 0.1) + 0.2, 1e-12);  // 0.8
    // 总时长 0.8：叶子 i 起点 = i*0.3。
    for (int i = 0; i < 3; ++i) {
        AURORA_TEST_CHECK_NEAR(tl.interval(static_cast<std::size_t>(i)).begin, i * 0.3 / 0.8, 1e-12);
        AURORA_TEST_CHECK_NEAR(tl.interval(static_cast<std::size_t>(i)).end, (i * 0.3 + 0.2) / 0.8, 1e-12);
    }

    // gap=0：等价 sequence（首尾相接）。
    const auto no_gap = aurora::TimelineSpec::staggered(0.2, 0.0, 3).build();
    AURORA_TEST_CHECK_NEAR(no_gap.interval(1).begin, no_gap.interval(0).end, 1e-12);
    AURORA_TEST_CHECK_NEAR(no_gap.duration(), 0.6, 1e-12);
}

/// @brief 嵌套组：sequence 内嵌 parallel（组整体平移到游标）+ 深度优先槽位序。
AURORA_TEST_CASE(timeline_nested_sequence_over_parallel) {
    auto par = aurora::TimelineSpec::parallel();
    par.add(0.3).add(0.5);
    const auto tl = aurora::TimelineSpec::sequence().add(0.2).add(par).add(0.1).build();

    // 总时长 = 0.2 + max(0.3,0.5) + 0.1 = 0.8。
    AURORA_TEST_CHECK_EQ(tl.slot_count(), std::size_t{4});
    AURORA_TEST_CHECK_NEAR(tl.duration(), 0.8, 1e-12);
    // 槽位深度优先序：0=首叶子，1/2=parallel 两叶子，3=尾叶子。
    AURORA_TEST_CHECK_NEAR(tl.interval(0).begin, 0.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(0).end, 0.25, 1e-12);   // 0.2/0.8
    AURORA_TEST_CHECK_NEAR(tl.interval(1).begin, 0.25, 1e-12); // parallel 锚定 0.2s
    AURORA_TEST_CHECK_NEAR(tl.interval(1).end, 0.25 + 0.3 / 0.8, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(2).begin, 0.25, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(2).end, 0.875, 1e-12);  // (0.2+0.5)/0.8
    AURORA_TEST_CHECK_NEAR(tl.interval(3).begin, 0.875, 1e-12);
    AURORA_TEST_CHECK_NEAR(tl.interval(3).end, 1.0, 1e-12);

    // 组时长独立可查（嵌套 spec 自身）。
    AURORA_TEST_CHECK_NEAR(par.duration(), 0.5, 1e-12);
}

/// @brief 空 spec / 非法时长：duration 夹取与空表防御。
AURORA_TEST_CASE(timeline_empty_and_invalid_duration_edges) {
    // 空 sequence：无槽位、时长 0。
    const auto empty = aurora::TimelineSpec::sequence().build();
    AURORA_TEST_CHECK_EQ(empty.slot_count(), std::size_t{0});
    AURORA_TEST_CHECK_NEAR(empty.duration(), 0.0, 1e-12);

    // staggered count=0：同空。
    const auto no_items = aurora::TimelineSpec::staggered(0.2, 0.1, 0).build();
    AURORA_TEST_CHECK_EQ(no_items.slot_count(), std::size_t{0});

    // 负时长叶子：夹取 1e-6，不产生负区间。
    const auto neg = aurora::TimelineSpec::sequence().add(-1.0).add(0.5).build();
    AURORA_TEST_CHECK_EQ(neg.slot_count(), std::size_t{2});
    AURORA_TEST_CHECK_NEAR(neg.interval(0).begin, 0.0, 1e-12);
    AURORA_TEST_CHECK_TRUE(neg.interval(0).end >= 0.0);
    AURORA_TEST_CHECK_NEAR(neg.interval(1).end, 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(neg.duration(), 0.5 + 1e-6, 1e-9);

    // staggered 负 gap 夹取为 0。
    const auto neg_gap = aurora::TimelineSpec::staggered(0.1, -5.0, 2).build();
    AURORA_TEST_CHECK_NEAR(neg_gap.duration(), 0.2, 1e-12);
}

}  // namespace aurora::test_cases::utest_timeline
