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
// 数字小键盘（Keypad）：X11 的 KP_* keysym 与主键各占独立码点，故本后端**天然可区分**，
// 全量映射（Win32 侧需靠 lParam 扫描码区分，见 `win32_modifiers.h` 的 `is_numpad_nav_scan`）。
// `KP_Enter`（0xFF8D）按既有决定并入 `KeyCode::Enter`，不在此重复列出。
constexpr unsigned long AURORA_KEYSYM_KP_0 = 0xFFB0, AURORA_KEYSYM_KP_9 = 0xFFB9;
constexpr unsigned long AURORA_KEYSYM_KP_Add = 0xFFAB, AURORA_KEYSYM_KP_Subtract = 0xFFAD,
                        AURORA_KEYSYM_KP_Multiply = 0xFFAA, AURORA_KEYSYM_KP_Divide = 0xFFAF,
                        AURORA_KEYSYM_KP_Decimal = 0xFFAE, AURORA_KEYSYM_KP_Separator = 0xFFAC;
constexpr unsigned long AURORA_KEYSYM_KP_Insert = 0xFF9E, AURORA_KEYSYM_KP_Delete = 0xFF9F,
                        AURORA_KEYSYM_KP_Begin = 0xFF9D, AURORA_KEYSYM_KP_Equal = 0xFFBD;
constexpr unsigned long AURORA_KEYSYM_KP_Home = 0xFF95, AURORA_KEYSYM_KP_End = 0xFF9C, AURORA_KEYSYM_KP_Prior = 0xFF9A,
                        AURORA_KEYSYM_KP_Next = 0xFF9B;
}  // namespace keysym

/// @brief keysym（X11 或 XKB，数值相同）→ 平台无关 KeyCode。
inline auto keysym_to_keycode(unsigned long ks) -> KeyCode {
    if (ks >= keysym::AURORA_KEYSYM_a && ks <= keysym::AURORA_KEYSYM_z) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + static_cast<int>(ks - keysym::AURORA_KEYSYM_a));
    }
    if (ks >= keysym::AURORA_KEYSYM_A && ks <= keysym::AURORA_KEYSYM_Z) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + static_cast<int>(ks - keysym::AURORA_KEYSYM_A));
    }
    if (ks >= keysym::AURORA_KEYSYM_0 && ks <= keysym::AURORA_KEYSYM_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::D0) + static_cast<int>(ks - keysym::AURORA_KEYSYM_0));
    }
    if (ks >= keysym::AURORA_KEYSYM_F1 && ks <= keysym::AURORA_KEYSYM_F12) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + static_cast<int>(ks - keysym::AURORA_KEYSYM_F1));
    }
    // KP_0-9 在 X11 是连号区间（0xFFB0..0xFFB9），与主键盘数字行同形，故走区间判而非逐个 case。
    if (ks >= keysym::AURORA_KEYSYM_KP_0 && ks <= keysym::AURORA_KEYSYM_KP_9) {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::KP_0) +
                                    static_cast<int>(ks - keysym::AURORA_KEYSYM_KP_0));
    }
    switch (ks) {
        case keysym::AURORA_KEYSYM_Return:
        case keysym::AURORA_KEYSYM_KP_Enter:
            return KeyCode::Enter;
        case keysym::AURORA_KEYSYM_Escape:
            return KeyCode::Escape;
        case keysym::AURORA_KEYSYM_Tab:
            return KeyCode::Tab;
        case keysym::AURORA_KEYSYM_BackSpace:
            return KeyCode::Backspace;
        case keysym::AURORA_KEYSYM_Delete:
            return KeyCode::Delete;
        case keysym::AURORA_KEYSYM_space:
            return KeyCode::Space;
        case keysym::AURORA_KEYSYM_Left:
            return KeyCode::ArrowLeft;
        case keysym::AURORA_KEYSYM_Right:
            return KeyCode::ArrowRight;
        case keysym::AURORA_KEYSYM_Up:
            return KeyCode::ArrowUp;
        case keysym::AURORA_KEYSYM_Down:
            return KeyCode::ArrowDown;
        case keysym::AURORA_KEYSYM_Shift_L:
        case keysym::AURORA_KEYSYM_Shift_R:
            return KeyCode::Shift;
        case keysym::AURORA_KEYSYM_Control_L:
        case keysym::AURORA_KEYSYM_Control_R:
            return KeyCode::Control;
        case keysym::AURORA_KEYSYM_Alt_L:
        case keysym::AURORA_KEYSYM_Alt_R:
            return KeyCode::Alt;
        case keysym::AURORA_KEYSYM_Super_L:
        case keysym::AURORA_KEYSYM_Super_R:
            return KeyCode::Meta;
        case keysym::AURORA_KEYSYM_Home:
            return KeyCode::Home;
        case keysym::AURORA_KEYSYM_End:
            return KeyCode::End;
        case keysym::AURORA_KEYSYM_Prior:
            return KeyCode::PageUp;
        case keysym::AURORA_KEYSYM_Next:
            return KeyCode::PageDown;
        case keysym::AURORA_KEYSYM_minus:
            return KeyCode::Minus;
        case keysym::AURORA_KEYSYM_equal:
            return KeyCode::Equal;
        case keysym::AURORA_KEYSYM_bracket_left:
            return KeyCode::LeftBracket;
        case keysym::AURORA_KEYSYM_bracket_right:
            return KeyCode::RightBracket;
        case keysym::AURORA_KEYSYM_backslash:
            return KeyCode::Backslash;
        case keysym::AURORA_KEYSYM_semicolon:
            return KeyCode::Semicolon;
        case keysym::AURORA_KEYSYM_apostrophe:
            return KeyCode::Quote;
        case keysym::AURORA_KEYSYM_comma:
            return KeyCode::Comma;
        case keysym::AURORA_KEYSYM_period:
            return KeyCode::Period;
        case keysym::AURORA_KEYSYM_slash:
            return KeyCode::Slash;
        case keysym::AURORA_KEYSYM_grave:
            return KeyCode::Backquote;
        // ---- 数字小键盘 ----
        // KP_0-9 在 X11 是连号区间（0xFFB0..0xFFB9），与主键盘数字行的区间判据同形。
        case keysym::AURORA_KEYSYM_KP_Insert:
            return KeyCode::KP_Insert;
        case keysym::AURORA_KEYSYM_KP_Delete:
            return KeyCode::KP_Delete;
        case keysym::AURORA_KEYSYM_KP_Begin:
            return KeyCode::KP_Begin;
        case keysym::AURORA_KEYSYM_KP_Home:
            return KeyCode::KP_Home;
        case keysym::AURORA_KEYSYM_KP_End:
            return KeyCode::KP_End;
        case keysym::AURORA_KEYSYM_KP_Prior:
            return KeyCode::KP_Prior;
        case keysym::AURORA_KEYSYM_KP_Next:
            return KeyCode::KP_Next;
        case keysym::AURORA_KEYSYM_KP_Add:
            return KeyCode::KP_Add;
        case keysym::AURORA_KEYSYM_KP_Subtract:
            return KeyCode::KP_Subtract;
        case keysym::AURORA_KEYSYM_KP_Multiply:
            return KeyCode::KP_Multiply;
        case keysym::AURORA_KEYSYM_KP_Divide:
            return KeyCode::KP_Divide;
        case keysym::AURORA_KEYSYM_KP_Decimal:
            return KeyCode::KP_Decimal;
        case keysym::AURORA_KEYSYM_KP_Separator:
            return KeyCode::KP_Separator;
        // KP_Equal（0xFFBD）在 KeyCode 里无对应码位（小键盘等号无独立语义），故不映射，
        // 与其它后端对「本枚举未收录的键」的处理一致（落 Unknown，不臆造码位）。
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace aurora::detail
