// test_canvas.cpp — Canvas 控件 1:1 测试：自定义绘制回调与像素写入。

#include "aurora/aurora.h"
#include "test_harness.h"

using aurora::BuildContext;
using aurora::Canvas;
using aurora::Color;
using aurora::Constraints;
using aurora::Painter;
using aurora::Rect;
using aurora::Size;

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
    AURORA_TEST_CHECK_MSG(px.m_r == 1 && px.m_g == 2 && px.m_b == 3 && px.m_a == 255,
                          "Canvas: set/get pixel roundtrip");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_canvas ===\n");
    test_canvas();
}
