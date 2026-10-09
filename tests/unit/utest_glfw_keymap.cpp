/// @file utest_glfw_keymap.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/glfw_keymap.h
/// 测试说明: GLFW 键码 → `KeyCode` 的映射表。字母 / 数字行 / 导航 / 标点 / 功能键与数字小键盘的
///           既有条目逐个锁定，另钉主键盘 `Insert`（`GLFW_KEY_INSERT`）与「GLFW 无 `KP_INSERT`
///           常量故导航区恒给主键码」这两条新增口径
/// 平台门控: 依赖 `AURORA_BACKEND_GLFW`（GLFW 键码常量与头随门控裁掉）；未开启时每条用例落 SKIP 桩
///           （声明无条件可见，满足 `runner --list` 与测试源字面量一致）
///
/// 为什么要有本文件：`from_glfw_key` 原先是 `glfw_surface.cpp` 里的 `static`（内部链接），
/// 单测吃不到表 —— 「四后端键码一致」这条契约在 Win32（`win32_keymap.h`）与 X11/Wayland
/// （`keysym_map.h`）都有 CTest 断言，唯独 GLFW 配置没有，错位只能等真机才显形。

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#ifdef AURORA_BACKEND_GLFW
#include <GLFW/glfw3.h>

#include "aurora/window/detail/glfw_keymap.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glfw_keymap {

#define AURORA_GLFW_KEYMAP_SKIP \
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW is not enabled: detail/glfw_keymap.h is compiled out")

AURORA_TEST_CASE(glfw_key_maps_letters_digits_and_navigation) {
#ifdef AURORA_BACKEND_GLFW
    using detail::from_glfw_key;
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_A) == KeyCode::A);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_Z) == KeyCode::Z);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_0) == KeyCode::D0);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_9) == KeyCode::D9);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_ESCAPE) == KeyCode::Escape);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_ENTER) == KeyCode::Enter);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_TAB) == KeyCode::Tab);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_BACKSPACE) == KeyCode::Backspace);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_DELETE) == KeyCode::Delete);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_SPACE) == KeyCode::Space);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_HOME) == KeyCode::Home);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_END) == KeyCode::End);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_PAGE_UP) == KeyCode::PageUp);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_PAGE_DOWN) == KeyCode::PageDown);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_F1) == KeyCode::F1);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_F12) == KeyCode::F12);
#else
    AURORA_GLFW_KEYMAP_SKIP;
#endif
}

// 数字小键盘：`KP_0-9` 在 GLFW 是连号区间（320..329），与主键盘数字行同形走区间判；
// 四则运算与小数点各有独立常量。`KP_Enter` 按既有决定并入 `KeyCode::Enter`。
AURORA_TEST_CASE(glfw_key_maps_the_keypad_range_and_operators) {
#ifdef AURORA_BACKEND_GLFW
    using detail::from_glfw_key;
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_0) == KeyCode::KP_0);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_5) == KeyCode::KP_5);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_9) == KeyCode::KP_9);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_ADD) == KeyCode::KP_Add);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_SUBTRACT) == KeyCode::KP_Subtract);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_MULTIPLY) == KeyCode::KP_Multiply);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_DIVIDE) == KeyCode::KP_Divide);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_DECIMAL) == KeyCode::KP_Decimal);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_ENTER) == KeyCode::Enter);
#else
    AURORA_GLFW_KEYMAP_SKIP;
#endif
}

// 主键盘 Insert：`GLFW_KEY_INSERT`（260）是 GLFW 键码表里**唯一**的 Insert 常量——没有
// `GLFW_KEY_KP_INSERT`，小键盘导航区与主键盘导航区被合并成同一组常量，故本后端恒给主键码、
// 恒不产 `KP_Insert`。与 Win32 侧 `VK_INSERT` 同口径：都能收到主键盘 Insert，都不做来处二次判定。
AURORA_TEST_CASE(glfw_key_maps_insert_to_the_mainboard_code) {
#ifdef AURORA_BACKEND_GLFW
    using detail::from_glfw_key;
    AURORA_TEST_CHECK(GLFW_KEY_INSERT == 260);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_INSERT) == KeyCode::Insert);
    // 不得串到小键盘档（GLFW 无对应常量，任何 KP_Insert 产出都属臆造）。
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_INSERT) != KeyCode::KP_Insert);
    // 与相邻的 Delete 互不串码。
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_DELETE) == KeyCode::Delete);
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_DELETE) != KeyCode::Insert);
#else
    AURORA_GLFW_KEYMAP_SKIP;
#endif
}

AURORA_TEST_CASE(glfw_key_falls_back_to_unknown_for_unmapped_codes) {
#ifdef AURORA_BACKEND_GLFW
    using detail::from_glfw_key;
    // `GLFW_KEY_KP_EQUAL`（336）在 `KeyCode` 里无对应码位（小键盘等号无独立语义），不臆造。
    AURORA_TEST_CHECK(from_glfw_key(GLFW_KEY_KP_EQUAL) == KeyCode::Unknown);
    AURORA_TEST_CHECK(from_glfw_key(-1) == KeyCode::Unknown);
#else
    AURORA_GLFW_KEYMAP_SKIP;
#endif
}

}  // namespace aurora::test_cases::utest_glfw_keymap
