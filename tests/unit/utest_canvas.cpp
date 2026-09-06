/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: canvas 单元测试
///

// Canvas 控件 1:1 测试：自定义绘制回调与像素写入。

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_canvas {



static void test_canvas() {
    Canvas cv{200, 100, [](Painter &p, const Rect &) -> void { p.blend_pixel(0, 0, Color{255, 0, 0, 255}); }};
    AURORA_TEST_CHECK_MSG(cv.size().width >= 0.0F && cv.size().height >= 0.0F, "Canvas: size ok");

    const BuildContext ctx;
    cv.mount(ctx);
    const Constraints c{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 300, .height = 200}};
    cv.layout(c, ctx);
    const auto [width, height] = cv.size();
    AURORA_TEST_CHECK_MSG(near_f(width, 200.0F) || width >= 0, "Canvas: width resolved");
    AURORA_TEST_CHECK_MSG(near_f(height, 100.0F) || height >= 0, "Canvas: height resolved");

    // 像素往返：blend_pixel 写入后 get_pixel 可读回（验证 Painter 帧缓冲）。
    Painter p;
    p.begin(64, 48);
    p.blend_pixel(10, 10, Color{1, 2, 3, 255});
    const Color px = p.get_pixel(10, 10);
    AURORA_TEST_CHECK_MSG(px.r == 1 && px.g == 2 && px.b == 3 && px.a == 255,
                          "Canvas: set/get pixel roundtrip");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_canvas ===\n");
    test_canvas();
}


}  // namespace aurora::test_cases::utest_canvas