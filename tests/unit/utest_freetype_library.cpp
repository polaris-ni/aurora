/// 测试类型: unit
/// 目标单元: include/aurora/render/freetype_library.h
/// 测试说明: 覆盖 FT_Library 进程级单例的懒初始化可用性与访问幂等性，以及完整关闭字体子系统后
/// 可重新初始化并恢复可用——关闭一律走 shutdown_font_discovery()（先逐个释放 FT_Face 再销毁
/// FT_Library），不依赖用例进程隔离

#include "aurora/core/font.h"
#include "aurora/render/font_discovery.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/freetype_library.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_freetype_library {

AURORA_TEST_CASE(lazy_initialization_yields_usable_handle) { AURORA_TEST_CHECK_NOT_NULL(render::ft_library()); }

AURORA_TEST_CASE(repeated_access_returns_same_handle) {
    // 单线程 UI 下的进程级单例：重复访问不得重复初始化（否则字形缓存与 face 句柄全部失效）。
    AURORA_TEST_CHECK_EQ(render::ft_library(), render::ft_library());
}

AURORA_TEST_CASE(shutdown_then_access_reinitializes) {
    // 关闭必须走完整路径：ft_shutdown() 只销毁 FT_Library，不触碰 font_discovery 注册表里由它
    // 派生的 FT_Face——裸调会留下野 face，单进程顺序跑全量时污染后续所有字体用例（原生
    // SIGSEGV；wasm 下野字段被复用为垃圾函数指针 → call_indirect 失败，表现为
    // `table index is out of bounds` / `function signature mismatch`）。
    // shutdown_font_discovery() 先逐个 FT_Done_Face、清注册表与解析缓存、置未初始化标记，再
    // 销毁 library，故之后可安全重建。
    render::shutdown_font_discovery();

    const FT_Library after = render::ft_library();
    AURORA_TEST_CHECK_NOT_NULL(after);
    AURORA_TEST_CHECK_EQ(after, render::ft_library());

    // 回归守护：重建后必须真正可用——度量一次会走 face→字形加载全链，只验句柄非空不足以
    // 覆盖「发现层重建时是否仍握着旧 library 的 face」。
    const float width = render::FontEngine::measure_width("A", Font{});
    AURORA_TEST_CHECK_GT(width, 0.0F);
}

}  // namespace aurora::test_cases::utest_freetype_library
