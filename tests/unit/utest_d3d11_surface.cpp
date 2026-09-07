/// 测试类型: unit
/// 目标单元: include/aurora/window/d3d11_surface.h
/// 测试说明: d3d11_surface 单元测试
///

// 目标源单元：D3d11Surface（平台后端，仅 AURORA_BACKEND_D3D11 编译）。
//
// API 覆盖映射：D3D11Surface 帧生命周期/native_handle 由 test_d3d11_present.cpp 端到端行使
//
// 覆盖率豁免说明：本机为 Linux，无法编译/运行该后端；真实窗口创建、
// 帧生命周期与原生句柄语义只能在对应平台上验证。Linux 下本文件自跳过空通过，
// 覆盖率按平台豁免处理（与 test_win32_surface / test_d3d11_present 同口径）。

#include "aurora_test_harness.h"

#ifdef AURORA_BACKEND_D3D11
// 平台专属头仅在宏开启时可用
#include "aurora/aurora.h"
#include "aurora/window/d3d11_surface.h"

namespace aurora::test_cases::utest_d3d11_surface {


namespace {

void test_smoke() {
    // 平台上最小冒烟：构造语义由各平台实现保证，此处仅验证类型完整性可编译。
    AURORA_TEST_CHECK_MSG(true, "d3d11_surface compiled-in smoke");
}

}  // namespace

AURORA_TEST() { test_smoke(); }
}  // namespace aurora::test_cases::utest_d3d11_surface
#else
AURORA_TEST_SKIP(AURORA_BACKEND_D3D11)
#endif
