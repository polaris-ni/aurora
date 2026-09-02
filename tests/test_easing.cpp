#include <cmath>
#include <cstdio>
#include <functional>

#include "aurora/animation/easing.h"
#include "aurora/aurora.h"
#include "aurora/core/log.h"

#include "test_harness.h"

using aurora::Curve;
using aurora::CurveKind;
using aurora::Curves;

static void test_named_curves() {
    const auto cases = { Curves::linear(),        Curves::ease_in(),        Curves::ease_out(),
                         Curves::ease_in_out(),   Curves::ease_in_sine(),   Curves::ease_out_sine(),
                         Curves::ease_in_cubic(), Curves::ease_out_cubic(), Curves::bounce_out() };
    for (const auto &c : cases) {
        AURORA_TEST_CHECK_MSG(near_d(c.transform(0.0), 0.0), "curve: transform(0)==0");
        AURORA_TEST_CHECK_MSG(near_d(c.transform(1.0), 1.0), "curve: transform(1)==1");
        // 端点外夹紧
        AURORA_TEST_CHECK_MSG(near_d(c.transform(-0.5), 0.0), "curve: clamps t<0 to 0");
        AURORA_TEST_CHECK_MSG(near_d(c.transform(1.5), 1.0), "curve: clamps t>1 to 1");
    }
    // 单调性抽查
    AURORA_TEST_CHECK_MSG(Curves::ease_in().transform(0.5) < 0.5, "ease_in: concave (t=0.5 < 0.5 progress)");
    AURORA_TEST_CHECK_MSG(Curves::ease_out().transform(0.5) > 0.5, "ease_out: convex (t=0.5 > 0.5 progress)");
    AURORA_TEST_CHECK_MSG(near_d(Curves::bounce_out().transform(1.0), 1.0), "bounce_out: converges to 1");
}

static void test_custom_curve() {
    Curve const c(std::function([](double t) -> double { return t * t; }));
    AURORA_TEST_CHECK_MSG(c.kind() == CurveKind::Custom, "custom: kind is Custom");
    AURORA_TEST_CHECK_MSG(near_d(c.transform(0.5), 0.25), "custom: x^2 at 0.5 == 0.25");
    AURORA_TEST_CHECK_MSG(near_d(c.transform(2.0), 1.0), "custom: clamps >1 to 1");
    AURORA_TEST_CHECK_MSG(near_d(c.transform(-1.0), 0.0), "custom: clamps <0 to 0");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== easing ===\n");
    test_named_curves();
    test_custom_curve();
}
