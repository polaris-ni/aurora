/// 测试类型: unit
/// 目标单元: include/aurora/window/wasm_surface.h
/// 测试说明: WASM 后端类型契约 skip 桩——头整体被
/// AURORA_PLATFORM_WASM && AURORA_BACKEND_WASM 门控（含 <emscripten.h>），非 Emscripten 工具链无法编译

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)
#include <type_traits>

#include "aurora/window/wasm_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_wasm_surface {

AURORA_TEST_CASE(wasm_surface_type_contract) {
#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::WasmSurface>);
    static_assert(!std::is_copy_constructible_v<aurora::WasmSurface>);
    static_assert(!std::is_move_constructible_v<aurora::WasmSurface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::WasmSurface>);
#else
    AURORA_TEST_SKIP("WASM 后端仅在 AURORA_PLATFORM_WASM && AURORA_BACKEND_WASM（Emscripten 工具链）下编译");
#endif
}

AURORA_TEST_CASE(wasm_surface_os_dependent_paths_skipped) {
#if defined(AURORA_PLATFORM_WASM) && defined(AURORA_BACKEND_WASM)
    // 构造即在浏览器环境注册 Emscripten 事件回调，present 走 EM_ASM 上屏 Canvas，
    // 离开浏览器 rAF/宿主环境无从验证，属集成层覆盖范围。
    AURORA_TEST_SKIP("WasmSurface 依赖浏览器宿主（Canvas/rAF/Emscripten 回调），单测不触碰宿主环境");
#else
    AURORA_TEST_SKIP("WASM 后端仅在 AURORA_PLATFORM_WASM && AURORA_BACKEND_WASM（Emscripten 工具链）下编译");
#endif
}

}  // namespace aurora::test_cases::utest_wasm_surface
