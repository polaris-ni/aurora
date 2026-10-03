#pragma once

// Wayland per-output 缩放选择：**内部头**（与 `glfw_dpi.h` / `glfw_keymap.h` 同列于
// src/aurora/window/detail/，不进 include/），故「取哪个输出的缩放」这条决策可被单测直接钉住。
//
// 为什么要独立成头而不是留在 `wayland_surface.cpp` 里：那三条分支（协议版本退化 / 已 enter
// 取所在输出 / 未 enter 取全部输出）**全是纯整数运算**，是本后端最容易写错又最难在真机上
// 观察到的部分——它只在窗口**跨屏**时才会给出与旧模型不同的答案，而 CI 无头、单机验证时
// 根本没有第二块屏可拖。留在 `.cpp` 里它对本仓的 Windows 主线**零判据覆盖**，只能靠读代码
// 自证；抽成不依赖协议类型的纯函数后，Windows 构建即可单测（见 utest_wayland_output_scale）。
// 为什么输入用 `std::uintptr_t` 而非 `wl_output *`：让本头**不 include 任何 wayland 协议头**。
// 指针身份在实现侧转成整数键（`reinterpret_cast<std::uintptr_t>(out)`），测试侧直接用
// 1/2/3 之类的哨兵值——于是本头可在 Windows 上编译通过并被真实断言，而不是被
// `AURORA_BACKEND_WAYLAND` 门控裁掉后恒 SKIP（那种单测在 CI 上是空转的假绿）。
//
// 门控：**刻意不加** `AURORA_BACKEND_WAYLAND`。本头只含纯逻辑、不含任何平台 API，
// 门控它没有收益（省不下编译时间），代价却是让唯一能覆盖它的单测在非 Linux 平台退化为
// SKIP 桩。此处与 `glfw_dpi.h` 的差异是**有意的**，理由即上一条。
// 代价是它会被带进 Windows 构建——已核过 `check_platform_macros`（只禁裸平台宏进 `#if`
// 条件，本头无任何 `#if`）与 `check_arch_module_map`，均不因此判红。
#include <algorithm>
#include <cstdint>
#include <vector>

namespace aurora::detail {

/// @brief 单个 wl_output 的身份 + 缩放（身份为不透明整数键，不要求是指针）。
struct WaylandOutput {
    std::uintptr_t key = 0;  ///< 输出身份键（实现侧由 `wl_output*` 转来；0 视为无效）。
    int scale = 1;  ///< 该输出的缩放（`wl_output.scale` 事件值）。
};

/// @brief 按 per-output 模型选出本表面应使用的 buffer scale。
///
/// 三条分支（与合成器约束对齐，顺序不可换）：
///   1. `compositor_version < 3` ⇒ 恒 1。`wl_surface.set_buffer_scale` 是 v3 引入的，
///      低于 v3 的合成器不认这个请求，用任何 >1 的值都会导致缓冲区尺寸被误解。
///   2. 已 enter ⇒ 取**所在输出**的最大缩放。窗口可同时跨在两屏上（两个输出都 enter），
///      此时取最大者：合成器要求「按最大者渲染才不被拉伸」，取小者会被放大而模糊。
///      这是 per-output 模型的核心——不 enter 就取全部输出的最大值会让窗口落到低 DPI 屏时
///      仍按高 DPI 渲染（缩水的字 + 白占显存）。
///   3. 未 enter（map 前 / 全屏切输出途中）⇒ 退回全部输出的最大值，理由同上但方向相反：
///      此时猜 1x 会让高 DPI 屏上的**首帧**发糊，随后 enter 到达即纠正。
///
/// @param compositor_version 已绑定的 wl_compositor 版本号。
/// @param outputs 全部已知输出及其缩放（`wl_registry` 绑定 + `wl_output.scale` 维护）。
/// @param entered 本表面当前所在输出的身份键（`wl_surface.enter` / `leave` 维护）。
/// @return 选出的 buffer scale，恒 ≥ 1。
[[nodiscard]] inline auto select_wayland_buffer_scale(std::uint32_t compositor_version,
                                                      const std::vector<WaylandOutput> &outputs,
                                                      const std::vector<std::uintptr_t> &entered) -> int {
    if (compositor_version < 3U) {
        return 1;  // set_buffer_scale 需 wl_surface v3：不支持则退化 1x
    }
    if (!entered.empty()) {
        int want = 1;
        for (const std::uintptr_t key : entered) {
            for (const WaylandOutput &out : outputs) {
                if (out.key == key) {
                    want = std::max(want, out.scale);
                }
            }
        }
        return want;
    }
    int want = 1;
    for (const WaylandOutput &out : outputs) {
        want = std::max(want, out.scale);
    }
    return want;
}

}  // namespace aurora::detail
