#include <cmath>
#include <cstdio>
#include <vector>

#include "aurora/animation/timeline.h"
#include "aurora/aurora.h"
#include "aurora/core/log.h"

#include "test_harness.h"

using aurora::Curves;
using aurora::Keyframes;
using aurora::Point;
using aurora::Tween;

static void test_tween() {
    const Tween tw(0.0, 10.0);
    AURORA_TEST_CHECK_MSG(near_d(tw.value(0.0), 0.0), "Tween: value(0)==begin");
    AURORA_TEST_CHECK_MSG(near_d(tw.value(1.0), 10.0), "Tween: value(1)==end");
    AURORA_TEST_CHECK_MSG(near_d(tw.value(0.5), 5.0), "Tween: linear midpoint");
    AURORA_TEST_CHECK_MSG(near_d(tw.begin(), 0.0) && near_d(tw.end(), 10.0), "Tween: begin/end accessors");

    const Tween curved(0.0, 10.0, Curves::ease_in());
    AURORA_TEST_CHECK_MSG(curved.value(0.5) < 5.0, "Tween: ease_in shapes below linear");

    const Tween ptw(Point{ .x = 0.0, .y = 0.0 }, Point{ .x = 2.0, .y = 4.0 });
    const Point p = ptw.value(0.5);
    AURORA_TEST_CHECK_MSG(near_d(p.x, 1.0) && near_d(p.y, 2.0), "Tween<Point>: interpolates x,y");
}

static void test_keyframes() {
    const Keyframes<double> kf(
        { { { .time = 0.0, .value = 0.0 }, { .time = 0.5, .value = 10.0 }, { .time = 1.0, .value = 20.0 } } });
    AURORA_TEST_CHECK_MSG(near_d(kf.value(0.0), 0.0), "Keyframes: start endpoint");
    AURORA_TEST_CHECK_MSG(near_d(kf.value(1.0), 20.0), "Keyframes: end endpoint");
    AURORA_TEST_CHECK_MSG(near_d(kf.value(-1.0), 0.0), "Keyframes: before first -> first");
    AURORA_TEST_CHECK_MSG(near_d(kf.value(2.0), 20.0), "Keyframes: after last -> last");
    AURORA_TEST_CHECK_MSG(near_d(kf.value(0.5), 10.0), "Keyframes: exact stop");
    AURORA_TEST_CHECK_MSG(near_d(kf.value(0.25), 5.0), "Keyframes: linear between stops");

    // 乱序构造后按时间排序仍正确
    const Keyframes<double> unsorted(
        { { { .time = 1.0, .value = 20.0 }, { .time = 0.0, .value = 0.0 }, { .time = 0.5, .value = 10.0 } } });
    AURORA_TEST_CHECK_MSG(near_d(unsorted.value(0.5), 10.0), "Keyframes: unsorted input still sorts");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== timeline ===\n");
    test_tween();
    test_keyframes();
}
