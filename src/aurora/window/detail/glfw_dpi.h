#pragma once

// GLFW 内容坐标 ↔ aurora 逻辑 dp 的换算：**内部头**（与 `glfw_keymap.h` / `glfw_modifiers.h`
// 同列于 src/aurora/window/detail/，不进 include/），故换算本身可被单测直接钉住。
//
// 为什么要独立成头而不是留在 `glfw_surface.cpp` 里：GLFW 3.3 起窗口尺寸与光标位置用的都是
// **屏幕坐标**——在 DPI 感知进程里它就是物理像素。aurora 的窗口模型是「消费方只见逻辑 dp，
// 物理像素只出现在后端内部」（见 `include/aurora/window/surface.h`），两者只差一个 content
// scale。此前 `glfw_surface.cpp` 把 `glfwGetWindowSize` 的返回值**直接当 dp 用**，却又把
// `glfwGetWindowContentScale` 原样作为 `scale_factor()` 上报，于是同一时刻对外宣称
// 「size = 320 dp」与「scale = 1.5」，而窗口实际是 320 **物理像素** = 213 dp。
// 后果有两处：① 消费者按 `size * scale` 算帧缓冲必然对不上（`etest_smoke_render` 的 GLFW
// 腿恒红即由此）；② 100% DPI 下 scale 恰为 1.0，两种单位解读**重合**，该分叉在 CI 上完全
// 不显形。`Painter` 早已备好机制（`set_scale` + `begin(逻辑 dp)` 内部按 scale 分配物理缓冲），
// Win32 / D3D11 / X11 / Wayland 四个后端都调了，只 GLFW 漏掉——本头把换算收敛成可测的
// 两支纯函数，使「漏掉换算」这件事在单测层面可见。
//
// 门控与 `glfw_keymap.h` 同款：`AURORA_BACKEND_GLFW`。
#include "aurora/core/platform.h"

#ifdef AURORA_BACKEND_GLFW

namespace aurora::detail {

/// @brief 归一化缩放因子：非正数回落 1.0（与 `Painter::set_scale` 的守卫同口径）。
/// @param scale 原始缩放因子。
/// @return 大于 0 的缩放因子。
[[nodiscard]] constexpr auto normalized_scale(float scale) -> float { return scale > 0.0F ? scale : 1.0F; }

/// @brief 逻辑 dp → GLFW 屏幕坐标（物理像素）。
/// @param dp 逻辑尺寸（dp）。
/// @param scale 内容缩放因子（`glfwGetWindowContentScale`）；非正数按 1.0 处理。
/// @return 物理像素尺寸（四舍五入）。
[[nodiscard]] constexpr auto glfw_px_from_dp(int dp, float scale) -> int {
    return dp <= 0 ? 0 : static_cast<int>(static_cast<float>(dp) * normalized_scale(scale) + 0.5F);
}

/// @brief GLFW 屏幕坐标（物理像素）→ 逻辑 dp。
/// @param px 物理像素尺寸。
/// @param scale 内容缩放因子（`glfwGetWindowContentScale`）；非正数按 1.0 处理。
/// @return 逻辑尺寸（dp，四舍五入）。
[[nodiscard]] constexpr auto glfw_dp_from_px(int px, float scale) -> int {
    return px <= 0 ? 0 : static_cast<int>(static_cast<float>(px) / normalized_scale(scale) + 0.5F);
}

// ---- 换算的编译期基准（失败即换算写错，不等运行期发现）----
//
// 100% DPI：两种单位解读重合——这正是该分叉长期不显形的原因，故先钉住它必须**恒等**。
static_assert(glfw_px_from_dp(320, 1.0F) == 320);
static_assert(glfw_dp_from_px(320, 1.0F) == 320);
// 150% DPI：1 dp = 1.5 px，两个方向都要对上。
static_assert(glfw_px_from_dp(800, 1.5F) == 1200);
static_assert(glfw_dp_from_px(1200, 1.5F) == 800);
// 125% / 175% 这类非整数倍：四舍五入而非截断（截断会让 1 dp 的窗恒少 1 px，累积成尺寸漂移）。
static_assert(glfw_px_from_dp(101, 1.25F) == 126);
static_assert(glfw_px_from_dp(101, 1.75F) == 177);
// 非法 scale 不得把尺寸压成 0（否则窗口退化成不可见），与 Painter 的守卫同口径。
static_assert(glfw_px_from_dp(320, 0.0F) == 320);
static_assert(glfw_px_from_dp(320, -1.0F) == 320);
static_assert(glfw_dp_from_px(320, 0.0F) == 320);
// 非正输入钳到 0（调用方据此判「尺寸不可得」并回落，不做负数除法）。
static_assert(glfw_px_from_dp(0, 1.5F) == 0);
static_assert(glfw_dp_from_px(0, 1.5F) == 0);
static_assert(glfw_px_from_dp(-5, 1.5F) == 0);
// 往返一致：dp → px → dp 必须回到原值（否则每帧尺寸漂移，窗口会逐帧变小）。
static_assert(glfw_dp_from_px(glfw_px_from_dp(640, 1.5F), 1.5F) == 640);
static_assert(glfw_dp_from_px(glfw_px_from_dp(802, 1.25F), 1.25F) == 802);

}  // namespace aurora::detail

#endif  // AURORA_BACKEND_GLFW
