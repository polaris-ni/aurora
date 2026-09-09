/// 测试类型: unit
/// 目标单元: include/aurora/window/window_state.h
/// 测试说明: 覆盖 WindowState/WindowMode 枚举的 to_string 全枚举覆盖、
/// compute_window_state/compute_window_mode 的映射规则与优先级、两维状态正交性

#include <string>
#include <type_traits>

#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_state {

AURORA_TEST_CASE(window_state_to_string_covers_all_enumerators) {
    // 三个可见性枚举值都有非空且互异的人类可读名（调试/序列化契约）。
    const std::string visible = to_string(WindowState::Visible);
    const std::string occluded = to_string(WindowState::Occluded);
    const std::string hidden = to_string(WindowState::Hidden);
    AURORA_TEST_CHECK_EQ(visible, "Visible");
    AURORA_TEST_CHECK_EQ(occluded, "Occluded");
    AURORA_TEST_CHECK_EQ(hidden, "Hidden");
    AURORA_TEST_CHECK_NE(visible, occluded);
    AURORA_TEST_CHECK_NE(visible, hidden);
    AURORA_TEST_CHECK_NE(occluded, hidden);
}

AURORA_TEST_CASE(window_mode_to_string_covers_all_enumerators) {
    // 四个几何态枚举值都有非空且互异的人类可读名。
    const std::string normal = to_string(WindowMode::Normal);
    const std::string maximized = to_string(WindowMode::Maximized);
    const std::string minimized = to_string(WindowMode::Minimized);
    const std::string fullscreen = to_string(WindowMode::FullScreen);
    AURORA_TEST_CHECK_EQ(normal, "Normal");
    AURORA_TEST_CHECK_EQ(maximized, "Maximized");
    AURORA_TEST_CHECK_EQ(minimized, "Minimized");
    AURORA_TEST_CHECK_EQ(fullscreen, "FullScreen");
    AURORA_TEST_CHECK_NE(maximized, minimized);
    AURORA_TEST_CHECK_NE(minimized, fullscreen);
}

AURORA_TEST_CASE(compute_window_state_minimized_is_always_hidden) {
    // 最小化时无论是否前台激活，可见性一律为 Hidden。
    AURORA_TEST_CHECK_EQ(compute_window_state(true, true), WindowState::Hidden);
    AURORA_TEST_CHECK_EQ(compute_window_state(true, false), WindowState::Hidden);
}

AURORA_TEST_CASE(compute_window_state_active_flag_decides_visible_vs_occluded) {
    // 非最小化时由「是否前台激活」二分：激活 → Visible，失焦 → Occluded。
    AURORA_TEST_CHECK_EQ(compute_window_state(false, true), WindowState::Visible);
    AURORA_TEST_CHECK_EQ(compute_window_state(false, false), WindowState::Occluded);
}

AURORA_TEST_CASE(compute_window_mode_fullscreen_has_highest_priority) {
    // 全屏标志压倒最小化/最大化（规格注明优先级：全屏 > 最小化 > 最大化 > 普通）。
    AURORA_TEST_CHECK_EQ(compute_window_mode(false, false, true), WindowMode::FullScreen);
    AURORA_TEST_CHECK_EQ(compute_window_mode(true, false, true), WindowMode::FullScreen);
    AURORA_TEST_CHECK_EQ(compute_window_mode(true, true, true), WindowMode::FullScreen);
}

AURORA_TEST_CASE(compute_window_mode_minimized_outranks_maximized) {
    // 同时最小化且最大化（Win32 还原态竞态可产生）时按最小化报告。
    AURORA_TEST_CHECK_EQ(compute_window_mode(true, true, false), WindowMode::Minimized);
}

AURORA_TEST_CASE(compute_window_mode_maximized_then_normal_fallthrough) {
    // 仅最大化 → Maximized；三标志全无 → Normal。
    AURORA_TEST_CHECK_EQ(compute_window_mode(false, true, false), WindowMode::Maximized);
    AURORA_TEST_CHECK_EQ(compute_window_mode(false, false, false), WindowMode::Normal);
}

AURORA_TEST_CASE(state_and_mode_are_orthogonal_dimensions) {
    // 两维是不同类型（一个枚举放不下对方取值域）；同一窗口可同时「可见 + 最大化」，
    // 最小化则同时落在两维（Hidden + Minimized）——推导函数互不约束。
    static_assert(!std::is_same_v<WindowState, WindowMode>,
                  "WindowState 与 WindowMode 必须保持独立枚举类型（正交维度）");
    AURORA_TEST_CHECK_EQ(compute_window_state(false, true), WindowState::Visible);
    AURORA_TEST_CHECK_EQ(compute_window_mode(false, true, false), WindowMode::Maximized);
    AURORA_TEST_CHECK_EQ(compute_window_state(true, false), WindowState::Hidden);
    AURORA_TEST_CHECK_EQ(compute_window_mode(true, false, false), WindowMode::Minimized);
}

}  // namespace aurora::test_cases::utest_window_state
