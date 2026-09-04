// test_alignment.cpp — Alignment 九宫格对齐原点计算 1:1 测试。
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::Alignment;
using aurora::Size;

static void test_align_origin() {
    constexpr Size container{.width = 100.0F, .height = 100.0F};
    constexpr Size child{.width = 20.0F, .height = 20.0F};

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::TopLeft, child, container).x, 0.0F) &&
                              near_f(align_origin(Alignment::TopLeft, child, container).y, 0.0F),
                          "Alignment: TopLeft -> (0,0)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::TopCenter, child, container).x, 40.0F) &&
                              near_f(align_origin(Alignment::TopCenter, child, container).y, 0.0F),
                          "Alignment: TopCenter -> (40,0)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::TopRight, child, container).x, 80.0F) &&
                              near_f(align_origin(Alignment::TopRight, child, container).y, 0.0F),
                          "Alignment: TopRight -> (80,0)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::CenterLeft, child, container).x, 0.0F) &&
                              near_f(align_origin(Alignment::CenterLeft, child, container).y, 40.0F),
                          "Alignment: CenterLeft -> (0,40)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::Center, child, container).x, 40.0F) &&
                              near_f(align_origin(Alignment::Center, child, container).y, 40.0F),
                          "Alignment: Center -> (40,40)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::CenterRight, child, container).x, 80.0F) &&
                              near_f(align_origin(Alignment::CenterRight, child, container).y, 40.0F),
                          "Alignment: CenterRight -> (80,40)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::BottomLeft, child, container).x, 0.0F) &&
                              near_f(align_origin(Alignment::BottomLeft, child, container).y, 80.0F),
                          "Alignment: BottomLeft -> (0,80)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::BottomCenter, child, container).x, 40.0F) &&
                              near_f(align_origin(Alignment::BottomCenter, child, container).y, 80.0F),
                          "Alignment: BottomCenter -> (40,80)");

    AURORA_TEST_CHECK_MSG(near_f(align_origin(Alignment::BottomRight, child, container).x, 80.0F) &&
                              near_f(align_origin(Alignment::BottomRight, child, container).y, 80.0F),
                          "Alignment: BottomRight -> (80,80)");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_alignment ===\n");
    test_align_origin();
}
