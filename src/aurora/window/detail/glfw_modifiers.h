#pragma once

// GLFW 修饰键位折算：**内部头**（与 `win32_modifiers.h` / `x11_modifiers.h` 同列于
// src/aurora/window/detail/，不进 include/），故可被单测直接吃表。
//
// 为什么要独立成头而不是留在 `glfw_surface.cpp` 里：这张表有两个入口（键盘回调的 `mods` 形参、
// 指针回调的 `glfwGetKey` 折算），两处必须产出**同一种 Shift 语义**——一旦各写一份，
// 就会出现「按住 Shift 点按钮有 Alt 位、拖动时没有」这类只在真机才显形的分叉。收进可单测的
// 内部头后，同源这件事由单测机械校验（两个入口对同一组语义输入产出相同位集）。
//
// **两种入口的语义差别（务必分清，勿混用）**：
//   1. `glfw_mods_to_aurora(int mods)`：折 GLFW 回调**形参**给的 `GLFW_MOD_*` 掩码。
//      该掩码由平台后端在事件分发时写入，精确反映「该事件发生的那一刻」——与键盘路径同源，
//      首选此入口。
//   2. `glfw_cached_modifiers(GLFWwindow *)`：`on_cursor_pos` / `on_scroll` 两个回调的 GLFW
//      签名**不给** `mods` 形参（GLFW 只给 button / key / char 回调发掩码），只能退而读
//      `glfwGetKey`。**它读的是 GLFW 自己随 key 事件推进的缓存事件态、不是对系统的物理轮询**
//      （`third_party/glfw/docs/input.md`：「This function only returns cached key event
//      state. It does not poll the system for the current state of the key.」）——这一点是
//      本入口成立的前提，勿按「现场轮询」理解。
//
// 为何不用 GLFW 3.6 的窗口属性：仓库固定 GLFW 3.5.1（`third_party/glfw/CMakeLists.txt`），
// 该版本没有「读当前修饰态」的窗口属性（`GLFW_MODIFIERS` 在 3.6 开发分支与 master 上均
// 尚未存在，3.6 也未发布），不存在更直接的官方入口。
//
// 门控与 `glfw_surface.cpp` 同款：`AURORA_BACKEND_GLFW`。
#include "aurora/core/platform.h"

#ifdef AURORA_BACKEND_GLFW

#include <GLFW/glfw3.h>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief GLFW `GLFW_MOD_*` 掩码 → `ModifierKey`（回调形参入口）。
///
/// 掩码是 `int` 且高位为符号位，故整体包在 `*-signed-bitwise` 豁免内（与本文件既有写法一致）。
[[nodiscard]] constexpr auto glfw_mods_to_aurora(int mods) -> ModifierKey {
    auto m = ModifierKey::None;
    // NOLINTBEGIN(*-signed-bitwise)
    if ((mods & GLFW_MOD_SHIFT) != 0) {
        m = m | ModifierKey::Shift;
    }
    if ((mods & GLFW_MOD_CONTROL) != 0) {
        m = m | ModifierKey::Control;
    }
    if ((mods & GLFW_MOD_ALT) != 0) {
        m = m | ModifierKey::Alt;
    }
    if ((mods & GLFW_MOD_SUPER) != 0) {
        m = m | ModifierKey::Meta;
    }
    // NOLINTEND(*-signed-bitwise)
    return m;
}

/// @brief 八个修饰键码的按下态 → 修饰位集（左右归并的**纯**映射，不读 GLFW 状态）。
///
/// 左右归并的必要性同 Win32 侧：不归并则左右各占一位，注入事件与某些布局的左右配对会残留幻影，
/// 且消费方拿到「左 Shift」这种值还得自己再折一次。抽成纯函数（只读入参）故可被单测直接吃表，
/// 读缓存的壳（`glfw_cached_modifiers`）只负责取键态、判读全在这里。
///
/// @param left_shift 左 Shift 是否按下。
/// @param right_shift 右 Shift 是否按下。
/// @param left_control 左 Ctrl 是否按下。
/// @param right_control 右 Ctrl 是否按下。
/// @param left_alt 左 Alt 是否按下。
/// @param right_alt 右 Alt 是否按下。
/// @param left_super 左 Super（Win 键 / Command）是否按下。
/// @param right_super 右 Super 是否按下。
/// @return 四位修饰键的位集；任一位左右两侧任一按下即置位。
[[nodiscard]] constexpr auto glfw_key_states_to_modifiers(bool left_shift, bool right_shift, bool left_control,
                                                         bool right_control, bool left_alt, bool right_alt,
                                                         bool left_super, bool right_super) -> ModifierKey {
    auto m = ModifierKey::None;
    if (left_shift || right_shift) {
        m = m | ModifierKey::Shift;
    }
    if (left_control || right_control) {
        m = m | ModifierKey::Control;
    }
    if (left_alt || right_alt) {
        m = m | ModifierKey::Alt;
    }
    if (left_super || right_super) {
        m = m | ModifierKey::Meta;
    }
    return m;
}

/// @brief 读 GLFW 缓存态的修饰键位（指针回调入口；`on_cursor_pos` / `on_scroll` 专用）。
///
/// 逐个读**八个**键码（左/右各四），任一按下即置位——与 `glfw_mods_to_aurora` 的
/// 左/右归并口径一致，因此同一物理状态在两个入口下产出同一位集。
///
/// **只取四个可按住的位，不读 NumLock**：锁定位与指针手势无语义关系（无小键盘参与），
/// 而每次事件多读一次 `glfwGetKey` 是纯开销。键盘路径的 NumLock 由 `with_glfw_numlock`
/// 单独并入——故指针事件与键盘事件在本位上存在**已知宽度差异**，消费方只看四个可按住位时
/// 无需掩码，需要该位时也不该从指针事件取（口径见 codespec/specification/05-event-navigation.md §2.2.2）。
///
/// @param w 目标窗口。
/// @return 四位修饰键的位集；读不到的键按「未按下」处理，不静默假报「按下」。
[[nodiscard]] inline auto glfw_cached_modifiers(GLFWwindow *w) -> ModifierKey {
    // GLFW 平台实现对未支持的键码返回 GLFW_RELEASE（`glfwGetKey` 不会失败），故直接比 PRESS。
    const auto down = [w](int key) { return glfwGetKey(w, key) == GLFW_PRESS; };
    return glfw_key_states_to_modifiers(down(GLFW_KEY_LEFT_SHIFT), down(GLFW_KEY_RIGHT_SHIFT),
                                        down(GLFW_KEY_LEFT_CONTROL), down(GLFW_KEY_RIGHT_CONTROL),
                                        down(GLFW_KEY_LEFT_ALT), down(GLFW_KEY_RIGHT_ALT),
                                        down(GLFW_KEY_LEFT_SUPER), down(GLFW_KEY_RIGHT_SUPER));
}

}  // namespace aurora::detail

#endif  // AURORA_BACKEND_GLFW
