/// 测试类型: unit
/// 目标单元: include/aurora/widget/spacer.h
/// 测试说明: spacer 单元测试
///

// Spacer 控件 1:1 测试：弹性填充与布局。
#include <string>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_spacer {



static void test_spacer() {
    Spacer sp;
    constexpr BuildContext ctx;
    sp.mount(ctx);
    constexpr Constraints c{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 200}};
    sp.layout(c, ctx);
    const Size s = sp.size();
    AURORA_TEST_CHECK_MSG(near_f(s.width, 100.0F), "Spacer: width fills available");
    AURORA_TEST_CHECK_MSG(near_f(s.height, 200.0F), "Spacer: height fills available");

    Spacer sp2{true};
    constexpr BuildContext ctx2;
    sp2.mount(ctx2);
    sp2.layout(c, ctx2);
    AURORA_TEST_CHECK_MSG(sp2.size().width >= 0.0F, "Spacer: expand layout ok");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_spacer ===\n");
    test_spacer();
}


}  // namespace aurora::test_cases::utest_spacer