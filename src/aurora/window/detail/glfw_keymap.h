#pragma once

// GLFW 键码 → aurora KeyCode 的映射：**内部头**（与 `win32_keymap.h` / `glfw_modifiers.h`
// 同列于 src/aurora/window/detail/，不进 include/），故可被单测直接吃表。
//
// 为什么要独立成头而不是留在 `glfw_surface.cpp` 里：这张表是「跨后端键码对齐」的唯一实现，
// 消费方普遍持有 `KeyCode → 平台原生值` 的反向映射表，哪一端错位都只表现为「某个键没反应」，
// 极难定位。此前它是 `glfw_surface.cpp` 里的 `static` 函数（内部链接），单测**吃不到表**，
// 于是「四后端键码一致性」这条契约在 GLFW 那一腿上没有任何 CTest 断言——同样是键码表，
// Win32 有 `win32_keymap.h`、X11/Wayland 有 `keysym_map.h`，唯独 GLFW 缺一个可测入口。
// 收进本头后该腿由 `utest_glfw_keymap` 机械校验。
//
// 门控与 `glfw_surface.cpp` 同款：`AURORA_BACKEND_GLFW`。
#include "aurora/core/platform.h"

#ifdef AURORA_BACKEND_GLFW

#include <GLFW/glfw3.h>

#include "aurora/event/keycode.h"

namespace aurora::detail {

/// @brief GLFW 键码 → 平台无关 `KeyCode`。
/// @param key GLFW 键码（`GLFW_KEY_*` 常量，含主键盘数字行与字母行的 ASCII 值）。
/// @return 对应的 `KeyCode`；未收录的键返回 `KeyCode::Unknown`。
///
[[nodiscard]] inline auto from_glfw_key(int key) -> KeyCode {
    switch (key) {
        case GLFW_KEY_A:
            return KeyCode::A;
        case GLFW_KEY_B:
            return KeyCode::B;
        case GLFW_KEY_C:
            return KeyCode::C;
        case GLFW_KEY_D:
            return KeyCode::D;
        case GLFW_KEY_E:
            return KeyCode::E;
        case GLFW_KEY_F:
            return KeyCode::F;
        case GLFW_KEY_G:
            return KeyCode::G;
        case GLFW_KEY_H:
            return KeyCode::H;
        case GLFW_KEY_I:
            return KeyCode::I;
        case GLFW_KEY_J:
            return KeyCode::J;
        case GLFW_KEY_K:
            return KeyCode::K;
        case GLFW_KEY_L:
            return KeyCode::L;
        case GLFW_KEY_M:
            return KeyCode::M;
        case GLFW_KEY_N:
            return KeyCode::N;
        case GLFW_KEY_O:
            return KeyCode::O;
        case GLFW_KEY_P:
            return KeyCode::P;
        case GLFW_KEY_Q:
            return KeyCode::Q;
        case GLFW_KEY_R:
            return KeyCode::R;
        case GLFW_KEY_S:
            return KeyCode::S;
        case GLFW_KEY_T:
            return KeyCode::T;
        case GLFW_KEY_U:
            return KeyCode::U;
        case GLFW_KEY_V:
            return KeyCode::V;
        case GLFW_KEY_W:
            return KeyCode::W;
        case GLFW_KEY_X:
            return KeyCode::X;
        case GLFW_KEY_Y:
            return KeyCode::Y;
        case GLFW_KEY_Z:
            return KeyCode::Z;
        case GLFW_KEY_0:
            return KeyCode::D0;
        case GLFW_KEY_1:
            return KeyCode::D1;
        case GLFW_KEY_2:
            return KeyCode::D2;
        case GLFW_KEY_3:
            return KeyCode::D3;
        case GLFW_KEY_4:
            return KeyCode::D4;
        case GLFW_KEY_5:
            return KeyCode::D5;
        case GLFW_KEY_6:
            return KeyCode::D6;
        case GLFW_KEY_7:
            return KeyCode::D7;
        case GLFW_KEY_8:
            return KeyCode::D8;
        case GLFW_KEY_9:
            return KeyCode::D9;
        case GLFW_KEY_ESCAPE:
            return KeyCode::Escape;
        case GLFW_KEY_ENTER:
            return KeyCode::Enter;
        case GLFW_KEY_TAB:
            return KeyCode::Tab;
        case GLFW_KEY_BACKSPACE:
            return KeyCode::Backspace;
        case GLFW_KEY_DELETE:
            return KeyCode::Delete;
        case GLFW_KEY_SPACE:
            return KeyCode::Space;
        case GLFW_KEY_LEFT:
            return KeyCode::ArrowLeft;
        case GLFW_KEY_RIGHT:
            return KeyCode::ArrowRight;
        case GLFW_KEY_UP:
            return KeyCode::ArrowUp;
        case GLFW_KEY_DOWN:
            return KeyCode::ArrowDown;
        case GLFW_KEY_LEFT_SHIFT:
        case GLFW_KEY_RIGHT_SHIFT:
            return KeyCode::Shift;
        case GLFW_KEY_LEFT_CONTROL:
        case GLFW_KEY_RIGHT_CONTROL:
            return KeyCode::Control;
        case GLFW_KEY_LEFT_ALT:
        case GLFW_KEY_RIGHT_ALT:
            return KeyCode::Alt;
        case GLFW_KEY_LEFT_SUPER:
        case GLFW_KEY_RIGHT_SUPER:
            return KeyCode::Meta;
        case GLFW_KEY_HOME:
            return KeyCode::Home;
        case GLFW_KEY_END:
            return KeyCode::End;
        case GLFW_KEY_PAGE_UP:
            return KeyCode::PageUp;
        case GLFW_KEY_PAGE_DOWN:
            return KeyCode::PageDown;
        // `GLFW_KEY_INSERT`（260）是主键盘 Insert 的独立常量——GLFW 的键码表里**只有**它、
        // 没有 `GLFW_KEY_KP_INSERT`，故本后端恒给主键码 `Insert`、无从也无须区分来处。
        // 与 Win32 侧 `VK_INSERT` 同口径（都能收到主键盘 Insert，都不做来处二次判定），
        // X11 / Wayland 才分得出 `KP_Insert`。详见 `keycode.h` 的小键盘口径注释。
        case GLFW_KEY_INSERT:
            return KeyCode::Insert;
        case GLFW_KEY_MINUS:
            return KeyCode::Minus;
        case GLFW_KEY_EQUAL:
            return KeyCode::Equal;
        case GLFW_KEY_LEFT_BRACKET:
            return KeyCode::LeftBracket;
        case GLFW_KEY_RIGHT_BRACKET:
            return KeyCode::RightBracket;
        case GLFW_KEY_BACKSLASH:
            return KeyCode::Backslash;
        case GLFW_KEY_SEMICOLON:
            return KeyCode::Semicolon;
        case GLFW_KEY_APOSTROPHE:
            return KeyCode::Quote;
        case GLFW_KEY_COMMA:
            return KeyCode::Comma;
        case GLFW_KEY_PERIOD:
            return KeyCode::Period;
        case GLFW_KEY_SLASH:
            return KeyCode::Slash;
        case GLFW_KEY_GRAVE_ACCENT:
            return KeyCode::Backquote;
        case GLFW_KEY_F1:
            return KeyCode::F1;
        case GLFW_KEY_F2:
            return KeyCode::F2;
        case GLFW_KEY_F3:
            return KeyCode::F3;
        case GLFW_KEY_F4:
            return KeyCode::F4;
        case GLFW_KEY_F5:
            return KeyCode::F5;
        case GLFW_KEY_F6:
            return KeyCode::F6;
        case GLFW_KEY_F7:
            return KeyCode::F7;
        case GLFW_KEY_F8:
            return KeyCode::F8;
        case GLFW_KEY_F9:
            return KeyCode::F9;
        case GLFW_KEY_F10:
            return KeyCode::F10;
        case GLFW_KEY_F11:
            return KeyCode::F11;
        case GLFW_KEY_F12:
            return KeyCode::F12;
        default:
            break;
    }
    // ---- 数字小键盘 ----
    //
    // GLFW 把 `KP_0-9` 定为连号区间（320..329），与主键盘数字行同形，按区间判。
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::KP_0) + (key - GLFW_KEY_KP_0));
    }
    switch (key) {
        case GLFW_KEY_KP_DECIMAL:
            return KeyCode::KP_Decimal;
        case GLFW_KEY_KP_DIVIDE:
            return KeyCode::KP_Divide;
        case GLFW_KEY_KP_MULTIPLY:
            return KeyCode::KP_Multiply;
        case GLFW_KEY_KP_SUBTRACT:
            return KeyCode::KP_Subtract;
        case GLFW_KEY_KP_ADD:
            return KeyCode::KP_Add;
        // `GLFW_KEY_KP_ENTER` 按既有决定并入 `KeyCode::Enter`，与另两后端同口径。
        case GLFW_KEY_KP_ENTER:
            return KeyCode::Enter;
        // `GLFW_KEY_KP_EQUAL`（336）在 KeyCode 里无对应码位（小键盘等号无独立语义），不臆造。
        //
        // **导航六键在 GLFW 上完全无法区分**：GLFW 的键码表**没有** `GLFW_KEY_KP_INSERT` /
        // `_HOME` 之类的常量——它把小键盘导航区与主键盘导航区合并成同一组 `GLFW_KEY_HOME` /
        // `_END` / `_PAGE_UP` / `_PAGE_DOWN` / `_DELETE`，故本后端恒给主键码。`KP_Insert` /
        // `KP_Delete` / `KP_Begin` / `KP_End` / `KP_Home` / `KP_Prior` / `KP_Next` /
        // `KP_Separator` 这几项**在 GLFW 后端恒不产生**（X11 / Wayland 侧 keysym 分得开；
        // Win32 侧六个键里也只有 `Home` 可分）。这是 GLFW 库自身的键码表限制，属平台事实
        // 而非疏漏，详见 `keycode.h` 的小键盘口径注释。
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace aurora::detail

#endif  // AURORA_BACKEND_GLFW
