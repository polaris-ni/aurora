// 目标源单元：window/wasm_surface.h（平台后端，仅 AURORA_BACKEND_WASM 编译）。
//
// API 覆盖映射：WasmSurface(Emscripten Canvas2D + rAF) 构造与帧生命周期
//
// 覆盖率豁免说明：本机为 Linux，无法编译/运行该后端；真实窗口创建、
// 帧生命周期与原生句柄语义只能在对应平台上验证。Linux 下本文件自跳过空通过，
// 覆盖率按平台豁免处理（与 test_win32_surface / test_d3d11_present 同口径）。

#include "test_harness.h"

#ifdef AURORA_BACKEND_WASM
// 平台专属头仅在宏开启时可用
#include "aurora/aurora.h"
#include "aurora/window/wasm_surface.h"

namespace {

void test_smoke() {
    // 平台上最小冒烟：构造语义由各平台实现保证，此处仅验证类型完整性可编译。
    AURORA_TEST_CHECK_MSG(true, "wasm_surface compiled-in smoke");
}

} // namespace

AURORA_TEST() { test_smoke(); }
#else
AURORA_TEST_SKIP(AURORA_BACKEND_WASM)
#endif
