// keysym_map.h —— 内部共享 keysym → KeyCode 映射（仅库实现可见，不属公共 API）。
//
// X11（XK_*）与 XKB（XKB_KEY_*）的 keysym 数值同源、逐位相等；因此一份以整数码点
// 表达的映射即可同时服务 X11 与 Wayland 两个后端，消除此前两份逐行相同的 switch。
// 码点为 X11 协议稳定常量，无平台头依赖（避免 Wayland TU 被迫包含 <X11/keysym.h>）。
#pragma once

#include "aurora/event/keycode.h"

namespace aurora::detail {

// X11/XKB keysym 码点（协议稳定常量，十六进制取自 X11/keysym.h）。
namespace keysym {
constexpr unsigned long AURORA_KEYSYM_a = 0x61, AURORA_KEYSYM_z = 0x7A;
constexpr unsigned long AURORA_KEYSYM_A = 0x41, AURORA_KEYSYM_Z = 0x5A;
constexpr unsigned long AURORA_KEYSYM_0 = 0x30, AURORA_KEYSYM_9 = 0x39;
constexpr unsigned long AURORA_KEYSYM_F1 = 0xFFBE, AURORA_KEYSYM_F12 = 0xFFC9;
constexpr unsigned long AURORA_KEYSYM_Return = 0xFF0D, AURORA_KEYSYM_KP_Enter = 0xFF8D;
constexpr unsigned long AURORA_KEYSYM_Escape = 0xFF1B, AURORA_KEYSYM_Tab = 0xFF09, AURORA_KEYSYM_BackSpace = 0xFF08,
                        AURORA_KEYSYM_Delete = 0xFFFF;
constexpr unsigned long AURORA_KEYSYM_space = 0x20;
constexpr unsigned long AURORA_KEYSYM_Left = 0xFF51, AURORA_KEYSYM_Right = 0xFF53, AURORA_KEYSYM_Up = 0xFF52,
                        AURORA_KEYSYM_Down = 0xFF54;
constexpr unsigned long AURORA_KEYSYM_Shift_L = 0xFFE1, AURORA_KEYSYM_Shift_R = 0xFFE2;
constexpr unsigned long AURORA_KEYSYM_Control_L = 0xFFE3, AURORA_KEYSYM_Control_R = 0xFFE4;
constexpr unsigned long AURORA_KEYSYM_Alt_L = 0xFFE9, AURORA_KEYSYM_Alt_R = 0xFFEA;
constexpr unsigned long AURORA_KEYSYM_Super_L = 0xFFEB, AURORA_KEYSYM_Super_R = 0xFFEC;
constexpr unsigned long AURORA_KEYSYM_Home = 0xFF50, AURORA_KEYSYM_End = 0xFF57, AURORA_KEYSYM_Prior = 0xFF55,
                        AURORA_KEYSYM_Next = 0xFF56;
constexpr unsigned long AURORA_KEYSYM_minus = 0x2D, AURORA_KEYSYM_equal = 0x3D;
constexpr unsigned long AURORA_KEYSYM_bracket_left = 0x5B, AURORA_KEYSYM_bracket_right = 0x5D,
                        AURORA_KEYSYM_backslash = 0x5C;
constexpr unsigned long AURORA_KEYSYM_semicolon = 0x3B, AURORA_KEYSYM_apostrophe = 0x27;
constexpr unsigned long AURORA_KEYSYM_comma = 0x2C, AURORA_KEYSYM_period = 0x2E, AURORA_KEYSYM_slash = 0x2F,
                        AURORA_KEYSYM_grave = 0x60;
}  // namespace keysym

/// @brief keysym（X11 或 XKB，数值相同）→ 平台无关 KeyCode。
inline auto keysym_to_keycode(unsigned long ks) -> KeyCode {
    using namespace keysym;
    if (ks >= AURORA_KEYSYM_a && ks <= AURORA_KEYSYM_z) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + static_cast<int>(ks - AURORA_KEYSYM_a));
    }
    if (ks >= AURORA_KEYSYM_A && ks <= AURORA_KEYSYM_Z) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + static_cast<int>(ks - AURORA_KEYSYM_A));
    }
    if (ks >= AURORA_KEYSYM_0 && ks <= AURORA_KEYSYM_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::D0) + static_cast<int>(ks - AURORA_KEYSYM_0));
    }
    if (ks >= AURORA_KEYSYM_F1 && ks <= AURORA_KEYSYM_F12) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + static_cast<int>(ks - AURORA_KEYSYM_F1));
    }
    switch (ks) {
        case AURORA_KEYSYM_Return:
        case AURORA_KEYSYM_KP_Enter:
            return KeyCode::Enter;
        case AURORA_KEYSYM_Escape:
            return KeyCode::Escape;
        case AURORA_KEYSYM_Tab:
            return KeyCode::Tab;
        case AURORA_KEYSYM_BackSpace:
            return KeyCode::Backspace;
        case AURORA_KEYSYM_Delete:
            return KeyCode::Delete;
        case AURORA_KEYSYM_space:
            return KeyCode::Space;
        case AURORA_KEYSYM_Left:
            return KeyCode::ArrowLeft;
        case AURORA_KEYSYM_Right:
            return KeyCode::ArrowRight;
        case AURORA_KEYSYM_Up:
            return KeyCode::ArrowUp;
        case AURORA_KEYSYM_Down:
            return KeyCode::ArrowDown;
        case AURORA_KEYSYM_Shift_L:
        case AURORA_KEYSYM_Shift_R:
            return KeyCode::Shift;
        case AURORA_KEYSYM_Control_L:
        case AURORA_KEYSYM_Control_R:
            return KeyCode::Control;
        case AURORA_KEYSYM_Alt_L:
        case AURORA_KEYSYM_Alt_R:
            return KeyCode::Alt;
        case AURORA_KEYSYM_Super_L:
        case AURORA_KEYSYM_Super_R:
            return KeyCode::Meta;
        case AURORA_KEYSYM_Home:
            return KeyCode::Home;
        case AURORA_KEYSYM_End:
            return KeyCode::End;
        case AURORA_KEYSYM_Prior:
            return KeyCode::PageUp;
        case AURORA_KEYSYM_Next:
            return KeyCode::PageDown;
        case AURORA_KEYSYM_minus:
            return KeyCode::Minus;
        case AURORA_KEYSYM_equal:
            return KeyCode::Equal;
        case AURORA_KEYSYM_bracket_left:
            return KeyCode::LeftBracket;
        case AURORA_KEYSYM_bracket_right:
            return KeyCode::RightBracket;
        case AURORA_KEYSYM_backslash:
            return KeyCode::Backslash;
        case AURORA_KEYSYM_semicolon:
            return KeyCode::Semicolon;
        case AURORA_KEYSYM_apostrophe:
            return KeyCode::Quote;
        case AURORA_KEYSYM_comma:
            return KeyCode::Comma;
        case AURORA_KEYSYM_period:
            return KeyCode::Period;
        case AURORA_KEYSYM_slash:
            return KeyCode::Slash;
        case AURORA_KEYSYM_grave:
            return KeyCode::Backquote;
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace aurora::detail
