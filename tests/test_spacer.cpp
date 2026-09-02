// test_spacer.cpp — Spacer 控件 1:1 测试：弹性填充与布局。
#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::Size;
using aurora::Spacer;

static void test_spacer() {
    Spacer sp;
    constexpr BuildContext ctx;
    sp.mount(ctx);
    constexpr Constraints c{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 200 } };
    sp.layout(c, ctx);
    const Size s = sp.size();
    AURORA_TEST_CHECK_MSG(near_f(s.width, 100.0f), "Spacer: width fills available");
    AURORA_TEST_CHECK_MSG(near_f(s.height, 200.0f), "Spacer: height fills available");

    Spacer sp2{ true };
    constexpr BuildContext ctx2;
    sp2.mount(ctx2);
    sp2.layout(c, ctx2);
    AURORA_TEST_CHECK_MSG(sp2.size().width >= 0.0f, "Spacer: expand layout ok");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_spacer ===\n");
    test_spacer();
}
