/// @file utest_glfw_dpi.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/glfw_dpi.h
/// 测试说明: GLFW 屏幕坐标（物理像素）↔ aurora 逻辑 dp 的换算。100% DPI 必须恒等、150% 必须
///           恰好对上、非整数倍（125% / 175%）必须四舍五入而非截断、非法 scale 不得把尺寸压成 0、
///           dp → px → dp 往返必须回到原值（否则每帧尺寸漂移）
/// 平台门控: 依赖 `AURORA_BACKEND_GLFW`（换算头随门控裁掉）；未开启时每条用例落 SKIP 桩
///           （声明无条件可见，满足 `runner --list` 与测试源字面量一致）
///
/// 为什么要有本文件：GLFW 3.3 起窗口尺寸与光标位置用的都是**屏幕坐标**（DPI 感知进程里即
/// 物理像素），而 aurora 的窗口模型是「消费方只见逻辑 dp」。`glfw_surface.cpp` 曾把
/// `glfwGetWindowSize` 的返回值直接当 dp 用，却又把 `glfwGetWindowContentScale` 原样作为
/// `scale_factor()` 上报——同一时刻对外宣称「size = 320 dp」与「scale = 1.5」，而窗口实际是
/// 320 物理像素 = 213 dp。100% DPI 下 scale 恰为 1.0、两种单位解读重合，该分叉**完全不显形**，
/// 只能靠换算本身的机械校验兜住（接线是否漏掉由 `etest_smoke_render` 的 GLFW 腿判定）。

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#ifdef AURORA_BACKEND_GLFW
#include "aurora/window/detail/glfw_dpi.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glfw_dpi {

#define AURORA_GLFW_DPI_SKIP AURORA_TEST_SKIP("AURORA_BACKEND_GLFW is not enabled: detail/glfw_dpi.h is compiled out")

AURORA_TEST_CASE(hundred_percent_dpi_makes_both_units_identical) {
#ifdef AURORA_BACKEND_GLFW
    // 100% DPI 是该分叉长期不显形的原因：两种单位解读在此重合。故钉住它必须**恒等**，
    // 一旦有人把换算写成 1.0 硬编码以外的东西，这条会先转红。
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(320, 1.0F), 320);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(320, 1.0F), 320);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(1, 1.0F), 1);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(801, 1.0F), 801);
#else
    AURORA_GLFW_DPI_SKIP;
#endif
}

AURORA_TEST_CASE(scaled_dpi_maps_one_dp_to_scale_pixels) {
#ifdef AURORA_BACKEND_GLFW
    // 150%：1 dp = 1.5 px。请求 800x600 dp 的窗口必须是 1200x900 物理像素——
    // 这正是本条修复前后的分界（修复前 800 dp 只得到 800 px ＝ 533 dp）。
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(800, 1.5F), 1200);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(600, 1.5F), 900);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(1200, 1.5F), 800);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(900, 1.5F), 600);
    // 125% / 175% 这类非整数倍：四舍五入而非截断。截断会让每 dp 少不到 1 px，
    // 在 800 dp 量级上累积成肉眼可见的尺寸偏差。
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(800, 1.25F), 1000);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(101, 1.25F), 126);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(101, 1.75F), 177);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(126, 1.25F), 101);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(177, 1.75F), 101);
#else
    AURORA_GLFW_DPI_SKIP;
#endif
}

AURORA_TEST_CASE(illegal_scale_never_collapses_the_size_to_zero) {
#ifdef AURORA_BACKEND_GLFW
    // scale 读到 0 / 负数时按 1.0 处理（与 Painter::set_scale 的守卫同口径）：
    // 若直接除法或直接乘 0，窗口会退化成不可见尺寸，且这个分叉只在异常路径上出现。
    AURORA_TEST_CHECK_EQ(detail::normalized_scale(0.0F), 1.0F);
    AURORA_TEST_CHECK_EQ(detail::normalized_scale(-1.0F), 1.0F);
    AURORA_TEST_CHECK_EQ(detail::normalized_scale(1.5F), 1.5F);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(320, 0.0F), 320);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(320, -1.0F), 320);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(320, 0.0F), 320);
    // 非正输入钳到 0（调用方据此判「尺寸不可得」并回落，不做负数除法）。
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(0, 1.5F), 0);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(0, 1.5F), 0);
    AURORA_TEST_CHECK_EQ(detail::glfw_px_from_dp(-5, 1.5F), 0);
    AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(-5, 1.5F), 0);
#else
    AURORA_GLFW_DPI_SKIP;
#endif
}

AURORA_TEST_CASE(dp_px_round_trip_returns_to_the_original_dp) {
#ifdef AURORA_BACKEND_GLFW
    // 往返一致是「窗口不会逐帧变小」的机械保证：begin_frame 每帧都做一次
    // px → dp，若两个方向的取整不对称，窗口尺寸会单调漂移。
    for (const int dp : {1, 7, 100, 640, 801, 1024}) {
        AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(detail::glfw_px_from_dp(dp, 1.5F), 1.5F), dp);
        AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(detail::glfw_px_from_dp(dp, 1.25F), 1.25F), dp);
        AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(detail::glfw_px_from_dp(dp, 1.75F), 1.75F), dp);
        AURORA_TEST_CHECK_EQ(detail::glfw_dp_from_px(detail::glfw_px_from_dp(dp, 1.0F), 1.0F), dp);
    }
#else
    AURORA_GLFW_DPI_SKIP;
#endif
}

}  // namespace aurora::test_cases::utest_glfw_dpi
