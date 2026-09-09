/// 测试类型: unit
/// 目标单元: include/aurora/render/freetype_library.h
/// 测试说明: 覆盖 FT_Library 进程级单例的懒初始化可用性与访问幂等性，以及 ft_shutdown 后可重新初始化
/// （各用例进程隔离，关闭不影响其他测试文件）

#include "aurora/render/freetype_library.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_freetype_library {

AURORA_TEST_CASE(lazy_initialization_yields_usable_handle) { AURORA_TEST_CHECK_NOT_NULL(render::ft_library()); }

AURORA_TEST_CASE(repeated_access_returns_same_handle) {
    // 单线程 UI 下的进程级单例：重复访问不得重复初始化（否则字形缓存与 face 句柄全部失效）。
    AURORA_TEST_CHECK_EQ(render::ft_library(), render::ft_library());
}

AURORA_TEST_CASE(shutdown_then_access_reinitializes) {
    render::ft_shutdown();
    const FT_Library after = render::ft_library();
    AURORA_TEST_CHECK_NOT_NULL(after);
    AURORA_TEST_CHECK_EQ(after, render::ft_library());
}

}  // namespace aurora::test_cases::utest_freetype_library
