/// 测试类型: unit
/// 目标单元: include/aurora/core/debug.h
/// 测试说明: 渲染纯度守卫（g_paint_depth 进入/退出配对、嵌套、debug 门控下的 no-op）单元测试

#include "aurora/core/debug.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_debug {

AURORA_TEST() {
#ifdef AURORA_ENABLE_DEBUG
    // ---- 1. 测试起点不在绘制上下文中 ----
    AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 0);

    // ---- 2. PaintPurityGuard 进入 +1、退出 -1 ----
    {
        debug::PaintPurityGuard guard;
        AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 1);
    }
    AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 0);

    // ---- 3. 嵌套绘制（Drawer 等内部递归 paint）深度累加 ----
    {
        debug::PaintPurityGuard outer;
        AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 1);
        {
            debug::PaintPurityGuard inner;
            AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 2);
            {
                debug::PaintPurityGuard innermost;
                AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 3);
            }
            AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 2);
        }
        AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 1);
    }
    AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 0);

    // ---- 4. 在绘制上下文内 check_render_purity 通过（不触发纯度断言） ----
    {
        debug::PaintPurityGuard guard;
        AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 1);
        debug::check_render_purity();
        AURORA_TEST_CHECK(true);
    }

    // ---- 5. 守卫拷贝构造不额外增加深度（仅 RAII 语义由使用者保证配对） ----
    {
        debug::PaintPurityGuard a;
        AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 1);
    }
    AURORA_TEST_CHECK_EQ(debug::g_paint_depth, 0);
#else
    // Release：整套纯度机制被编译掉，check_render_purity 退化为 no-op。
    debug::check_render_purity();
    AURORA_TEST_CHECK(true);
#endif
}

}  // namespace aurora::test_cases::utest_debug
