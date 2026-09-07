/// 测试类型: unit
/// 目标单元: include/aurora/render/freetype_library.h
/// 测试说明: utest_freetype_library 单元测试
///

#include "aurora/aurora.h"
#include "aurora/render/freetype_library.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_freetype_library {

AURORA_TEST() {
    // 进程级 FT_Library 单例：首次调用懒初始化，返回非 null 句柄
    FT_Library lib = au::render::ft_library();
    AURORA_TEST_CHECK(lib != nullptr);
    // 重复调用返回同一进程级实例（仍非 null）
    AURORA_TEST_CHECK(au::render::ft_library() != nullptr);
    // 显式释放后可再次懒初始化
    au::render::ft_shutdown();
    AURORA_TEST_CHECK(au::render::ft_library() != nullptr);
}

}  // namespace aurora::test_cases::utest_freetype_library
