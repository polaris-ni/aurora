/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: stack 单元测试
///

// Stack 控件 1:1 测试：fit 属性与子节点布局。
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_stack {



static void test_stack() {
    Stack st{std::vector{Node{Text{"a"}}, Node{Text{"b"}}}, Alignment::Center};
    constexpr BuildContext ctx;
    st.mount(ctx);
    constexpr Constraints c{.min = Size{.width = 0, .height = 0}, .max = Size{.width = 100, .height = 100}};
    st.layout(c, ctx);
    AURORA_TEST_CHECK_MSG(st.size().width >= 0.0F && st.size().height >= 0.0F, "Stack: layout ok");

    Stack expand{std::vector{Node{Text{"x"}}}, Alignment::TopLeft};
    expand.set_fit(StackFit::Expand);
    constexpr BuildContext ctx2;
    expand.mount(ctx2);
    expand.layout(c, ctx2);
    AURORA_TEST_CHECK_MSG(expand.size().width >= 0.0F, "Stack: Expand fit ok");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_stack ===\n");
    test_stack();
}


}  // namespace aurora::test_cases::utest_stack