/// @file utest_wayland_output_scale.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/wayland_output_scale.h
/// 测试说明: Wayland per-output 缩放的选择决策。三条分支各自锁定：协议版本 < 3 必须退化 1x
///           （`set_buffer_scale` 是 wl_surface v3 才有的）、已 enter 必须取**所在输出**的最大值
///           （而非全部输出的最大值——这正是 per-output 模型的修正点）、未 enter 必须退回全部
///           输出的最大值（否则高 DPI 屏首帧发糊）、跨两屏时取两者较大者、未知身份键不得
///           抬到 2
/// 平台门控: **无**。被测头只含纯整数逻辑、不含任何平台 API，刻意不加
///           `AURORA_BACKEND_WAYLAND` 门控——门控它没有收益（省不下编译时间），代价却是让唯一
///           能覆盖它的单测在非 Linux 平台退化成 SKIP 桩。`utest_wayland_surface.cpp` 里那些
///           真·协议交互用例仍按 `AURORA_BACKEND_WAYLAND` 门控，两者并不冲突
///
/// 为什么要有本文件：该决策只在窗口**跨屏**时才与「取全部输出最大值」这个朴素做法给出不同
/// 答案，而 CI 无头、开发机单机，跨屏场景**无法自然复现**。留在 `wayland_surface.cpp` 里它对
/// Windows 主线零判据覆盖，只能靠读代码自证——而它恰恰是最容易写错的一处（两条分支的
/// 方向性错误对称：已 enter 误取全部输出 ⇒ 低 DPI 屏上按高 DPI 渲染；未 enter 误猜 1x ⇒ 高 DPI
/// 屏首帧发糊，两者都不会报错、只会静默地难看）。

#include "aurora/window/detail/wayland_output_scale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_wayland_output_scale {

namespace {

using detail::WaylandOutput;

/// @brief 两条输出：键 1 是 2x 屏，键 2 是 1x 屏（哨兵值即可，非真实指针）。
auto dual_output_table() -> std::vector<WaylandOutput> {
    return {WaylandOutput{.key = 1U, .scale = 2}, WaylandOutput{.key = 2U, .scale = 1}};
}

}  // namespace

AURORA_TEST_CASE(compositor_below_v3_degrades_to_one) {
    // set_buffer_scale 需 wl_surface v3。低于 v3 却递 >1，合成器要么忽略要么误算缓冲区尺寸，
    // 故必须无条件退化——即使所在输出报的是 2x。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(1U, dual_output_table(), {1U}), 1);
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(2U, dual_output_table(), {1U}), 1);
    // v3 起才按输出取值（1U 表示已绑定的 wl_compositor 最高版本被裁到 4，此处只验边界）。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(3U, dual_output_table(), {1U}), 2);
}

AURORA_TEST_CASE(entered_output_wins_over_global_maximum) {
    // per-output 模型的**核心修正点**：窗口落在 1x 屏上时必须按 1x 渲染。
    // 旧做法（取全部输出最大值）会在这里返回 2 ⇒ 低 DPI 屏上按高 DPI 渲染，字体缩小。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {2U}), 1);
    // 对照：落在 2x 屏上则应得 2。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {1U}), 2);
}

AURORA_TEST_CASE(not_entered_falls_back_to_global_maximum) {
    // map 前 / 全屏切输出途中都没有 enter。此时取 1x 会让高 DPI 屏上的**首帧**发糊，
    // 故退回全部输出的最大值，随后 enter 到达再纠正。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {}), 2);
    // 一个输出都没有时恒 1，不得返回 0（0 会被 set_buffer_scale 当非法值）。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, {}, {}), 1);
}

AURORA_TEST_CASE(spanning_two_monitors_takes_the_larger) {
    // 窗口同时跨在两屏上（两个输出都 enter）⇒ 取较大者：合成器要求按最大者渲染才不被拉伸，
    // 取小者会被放大而模糊。注意这与「取全局最大值」在本例中恰好同值，故另配一条反向用例。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {1U, 2U}), 2);
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {2U, 1U}), 2);
}

AURORA_TEST_CASE(unknown_output_key_never_inflates_the_scale) {
    // entered 里出现 outputs 未收录的身份键（bind 与 enter 的时序竞争：输出刚被移除、
    // enter 却仍在队列里）时，不得因此抬到 2，仍应退回该输出的真实值 1。
    AURORA_TEST_CHECK_EQ(detail::select_wayland_buffer_scale(4U, dual_output_table(), {99U}), 1);
}

}  // namespace aurora::test_cases::utest_wayland_output_scale
