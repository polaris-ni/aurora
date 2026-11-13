#pragma once

/// @brief macOS 原生键码 / 修饰位到平台无关 `KeyCode` / `ModifierKey` 的折算是纯函数层。
///
/// 与 `win32_keymap.h` / `x11_modifiers.h` 同一定位：原生值只在本层出现，Surface 事件翻译
/// 只消费 `KeyCode` / `ModifierKey`。本层不引 AppKit，故可在无窗口的单元测试里逐值断言。
/// 原生取值来源：Carbon `HIToolbox/Events.h` 的 `kVK_*` 常量与 `NSEventModifierFlag*`
/// （均为 ABI 稳定的整数常量，这里以字面量复刻以免把 Carbon 头拖进测试 TU）。
/// @file macos_keymap.h

#include <cstdint>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"

namespace aurora::detail {

/// @brief `NSEventModifierFlagShift`（AppKit 公开常量，1 << 17）。
inline constexpr std::uint64_t AURORA_MACOS_MOD_SHIFT = 1ULL << 17U;
/// @brief `NSEventModifierFlagControl`（1 << 18）。
inline constexpr std::uint64_t AURORA_MACOS_MOD_CONTROL = 1ULL << 18U;
/// @brief `NSEventModifierFlagOption`（1 << 19；语义同 Alt）。
inline constexpr std::uint64_t AURORA_MACOS_MOD_OPTION = 1ULL << 19U;
/// @brief `NSEventModifierFlagCommand`（1 << 20；语义同 Meta）。
inline constexpr std::uint64_t AURORA_MACOS_MOD_COMMAND = 1ULL << 20U;

/// @brief 把 `NSEvent.modifierFlags` 折算为框架修饰位集。
///
/// 只取框架建模的四个可按住位：Shift / Control / Option→Alt / Command→Meta。
/// CapsLock 与 Function 位不属 `ModifierKey`（前者是锁定态、后者是设备层），如实丢弃。
/// macOS 无 NumLock，故 `ModifierKey::NumLock` 恒不置位。
/// @param flags 原生修饰位（`NSEventModifierFlags` 的整数值）。
/// @return 框架修饰位并集。
[[nodiscard]] constexpr auto macos_modifiers_from_flags(std::uint64_t flags) -> ModifierKey {
    ModifierKey m = ModifierKey::None;
    if ((flags & AURORA_MACOS_MOD_SHIFT) != 0U) {
        m = m | ModifierKey::Shift;
    }
    if ((flags & AURORA_MACOS_MOD_CONTROL) != 0U) {
        m = m | ModifierKey::Control;
    }
    if ((flags & AURORA_MACOS_MOD_OPTION) != 0U) {
        m = m | ModifierKey::Alt;
    }
    if ((flags & AURORA_MACOS_MOD_COMMAND) != 0U) {
        m = m | ModifierKey::Meta;
    }
    return m;
}

/// @brief 把 `NSEvent.keyCode`（Carbon 虚拟键码）折算为平台无关逻辑键码。
///
/// 覆盖主键盘字母 / 数字 / 标点、功能键 F1–F12、方向键、编辑导航键、修饰键与小键盘。
/// 未收录的原生键（媒体键、亮度键、F13+ 等）一律 `KeyCode::Unknown`——不猜近义键。
/// 主键盘回车与小键盘回车都映射 `KeyCode::Enter`（框架口径，见 `keycode.h`）。
/// @param code Carbon 虚拟键码（`NSEvent.keyCode`）。
/// @return 对应逻辑键码；未映射时为 `KeyCode::Unknown`。
[[nodiscard]] constexpr auto macos_keycode_to_keycode(unsigned short code) -> KeyCode {
    switch (code) {
        // 字母（美式布局物理键位）
        case 0:
            return KeyCode::A;
        case 1:
            return KeyCode::S;
        case 2:
            return KeyCode::D;
        case 3:
            return KeyCode::F;
        case 4:
            return KeyCode::H;
        case 5:
            return KeyCode::G;
        case 6:
            return KeyCode::Z;
        case 7:
            return KeyCode::X;
        case 8:
            return KeyCode::C;
        case 9:
            return KeyCode::V;
        case 11:
            return KeyCode::B;
        case 12:
            return KeyCode::Q;
        case 13:
            return KeyCode::W;
        case 14:
            return KeyCode::E;
        case 15:
            return KeyCode::R;
        case 16:
            return KeyCode::Y;
        case 17:
            return KeyCode::T;
        case 31:
            return KeyCode::O;
        case 32:
            return KeyCode::U;
        case 34:
            return KeyCode::I;
        case 35:
            return KeyCode::P;
        case 37:
            return KeyCode::L;
        case 38:
            return KeyCode::J;
        case 40:
            return KeyCode::K;
        case 45:
            return KeyCode::N;
        case 46:
            return KeyCode::M;
        // 数字行
        case 18:
            return KeyCode::D1;
        case 19:
            return KeyCode::D2;
        case 20:
            return KeyCode::D3;
        case 21:
            return KeyCode::D4;
        case 22:
            return KeyCode::D6;
        case 23:
            return KeyCode::D5;
        case 25:
            return KeyCode::D9;
        case 26:
            return KeyCode::D7;
        case 28:
            return KeyCode::D8;
        case 29:
            return KeyCode::D0;
        // 标点
        case 24:
            return KeyCode::Equal;
        case 27:
            return KeyCode::Minus;
        case 30:
            return KeyCode::RightBracket;
        case 33:
            return KeyCode::LeftBracket;
        case 39:
            return KeyCode::Quote;
        case 41:
            return KeyCode::Semicolon;
        case 42:
            return KeyCode::Backslash;
        case 43:
            return KeyCode::Comma;
        case 44:
            return KeyCode::Slash;
        case 47:
            return KeyCode::Period;
        case 50:
            return KeyCode::Backquote;
        // 编辑 / 控制
        case 36:
            return KeyCode::Enter;  // Return
        case 48:
            return KeyCode::Tab;
        case 49:
            return KeyCode::Space;
        case 51:
            return KeyCode::Backspace;
        case 53:
            return KeyCode::Escape;
        case 117:
            return KeyCode::Delete;  // Forward Delete
        // 修饰键（左右两侧合并）
        case 55:
        case 54:
            return KeyCode::Meta;  // Command
        case 56:
        case 60:
            return KeyCode::Shift;
        case 58:
        case 61:
            return KeyCode::Alt;  // Option
        case 59:
        case 62:
            return KeyCode::Control;
        // 方向键
        case 123:
            return KeyCode::ArrowLeft;
        case 124:
            return KeyCode::ArrowRight;
        case 125:
            return KeyCode::ArrowDown;
        case 126:
            return KeyCode::ArrowUp;
        // 导航
        case 115:
            return KeyCode::Home;
        case 119:
            return KeyCode::End;
        case 116:
            return KeyCode::PageUp;
        case 121:
            return KeyCode::PageDown;
        // 功能键
        case 122:
            return KeyCode::F1;
        case 120:
            return KeyCode::F2;
        case 99:
            return KeyCode::F3;
        case 118:
            return KeyCode::F4;
        case 96:
            return KeyCode::F5;
        case 97:
            return KeyCode::F6;
        case 98:
            return KeyCode::F7;
        case 100:
            return KeyCode::F8;
        case 101:
            return KeyCode::F9;
        case 109:
            return KeyCode::F10;
        case 103:
            return KeyCode::F11;
        case 111:
            return KeyCode::F12;
        // 小键盘
        case 82:
            return KeyCode::KP_0;
        case 83:
            return KeyCode::KP_1;
        case 84:
            return KeyCode::KP_2;
        case 85:
            return KeyCode::KP_3;
        case 86:
            return KeyCode::KP_4;
        case 87:
            return KeyCode::KP_5;
        case 88:
            return KeyCode::KP_6;
        case 89:
            return KeyCode::KP_7;
        case 91:
            return KeyCode::KP_8;
        case 92:
            return KeyCode::KP_9;
        case 65:
            return KeyCode::KP_Decimal;
        case 67:
            return KeyCode::KP_Multiply;
        case 69:
            return KeyCode::KP_Add;
        case 75:
            return KeyCode::KP_Divide;
        case 76:
            return KeyCode::Enter;  // Keypad Enter 并入 Enter（框架口径）
        case 78:
            return KeyCode::KP_Subtract;
        case 81:
            // 小键盘等号（kVK_ANSI_KeypadEquals）：框架未单列 KP_Equal，按既有
            // 「KP_Enter 并入 Enter」的并档口径并入主键盘 Equal，不猜近义键。
            return KeyCode::Equal;
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace aurora::detail
