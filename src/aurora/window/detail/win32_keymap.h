#pragma once

// Win32 虚拟键码 → aurora KeyCode 的映射：**内部头**（与 `win32_modifiers.h` 同列于 src/，
// 不进 include/），故可被单测直接吃表。
//
// 为什么要独立成头而不是留在 `win32_host.cpp` 里：这张表是「跨后端键码对齐」的唯一实现，
// 消费方普遍持有 `KeyCode → 平台原生值` 的反向映射表，哪一端错位都只表现为「某个键没反应」，
// 极难定位。把它与「小键盘来处判据」一并放进可单测的内部头，那些错位就能在 CTest 里被钉住。
//
// 门控与 `win32_modifiers.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

#include "aurora/event/event.h"
#include "aurora/event/keycode.h"
#include "aurora/window/detail/win32_modifiers.h"

namespace aurora::detail {

/// @brief Win32 虚拟键码 → 平台无关 `KeyCode`。
/// @param vk 虚拟键码。
/// @param from_numpad 该键是否来自数字小键盘导航区（判据见 `is_numpad_nav_scan`）。
/// @return 对应的 `KeyCode`；未收录的键返回 `KeyCode::Unknown`。
///
[[nodiscard]] inline auto from_win32_vk(int vk, bool from_numpad) -> KeyCode {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<KeyCode>(static_cast<int>(KeyCode::D0) + (vk - '0'));
    }
    switch (vk) {
        case VK_RETURN:
            return KeyCode::Enter;
        case VK_ESCAPE:
            return KeyCode::Escape;
        case VK_TAB:
            return KeyCode::Tab;
        case VK_BACK:
            return KeyCode::Backspace;
        case VK_SPACE:
            return KeyCode::Space;
        // 小键盘中央那组倒 T 方向键（独立物理键，来处可由扫描码区分）在本枚举里**没有**对应
        // `KP_*` 码位——`KP_*` 集合只覆盖导航六键、运算键与 0-9。故此处与主键盘方向键同码，
        // 由消费方按 `modifiers & NumLock` 与具体场景自行判别，不另立语义。
        case VK_LEFT:
            return KeyCode::ArrowLeft;
        case VK_RIGHT:
            return KeyCode::ArrowRight;
        case VK_UP:
            return KeyCode::ArrowUp;
        case VK_DOWN:
            return KeyCode::ArrowDown;
        case VK_SHIFT:
            return KeyCode::Shift;
        case VK_CONTROL:
            return KeyCode::Control;
        case VK_MENU:
            return KeyCode::Alt;
        case VK_LWIN:
        case VK_RWIN:
            return KeyCode::Meta;
        // 导航区：Win32 在这一区把主键盘与小键盘**共用同一组 VK**，来处只在 lParam 的扫描码里。
        // 实测六个键里**只有 `Home` 真的可分**（主键盘 0x47 / 小键盘 E0 4E），其余五个键两边
        // 的 (VK, scan, extended) 完全相同或仅 extended 位不同，无法可靠判别（详见
        // `detail::is_numpad_nav_scan` 的对照表）。故只有 `Home` 走双分支，其余恒给主键码、
        // 维持现网行为——`KP_End` / `KP_Prior` / `KP_Next` / `KP_Delete` / `KP_Insert` 在本
        // 后端**恒不产生**，这是平台事实而非疏漏。
        case VK_HOME:
            return from_numpad ? KeyCode::KP_Home : KeyCode::Home;
        case VK_END:
            return KeyCode::End;
        case VK_PRIOR:
            return KeyCode::PageUp;
        case VK_NEXT:
            return KeyCode::PageDown;
        case VK_DELETE:
            return KeyCode::Delete;
        // `VK_INSERT` 维持既有行为（落 `default` → `Unknown`）：主键盘导航区有 Insert 而 Win32
        // 未给本库 `VK_INSERT` 分支，本轮不顺手补这个与 G16 无关的键。
        case VK_OEM_MINUS:
            return KeyCode::Minus;
        case VK_OEM_PLUS:
            return KeyCode::Equal;
        case VK_OEM_1:
            return KeyCode::Semicolon;
        case VK_OEM_7:
            return KeyCode::Quote;
        case VK_OEM_COMMA:
            return KeyCode::Comma;
        case VK_OEM_PERIOD:
            return KeyCode::Period;
        case VK_OEM_2:
            return KeyCode::Slash;
        case VK_OEM_3:
            return KeyCode::Backquote;
        case VK_OEM_4:
            return KeyCode::LeftBracket;
        case VK_OEM_6:
            return KeyCode::RightBracket;
        case VK_OEM_5:
            return KeyCode::Backslash;
        // ---- 数字小键盘 ----
        //
        // 两类来源：
        //   * `VK_NUMPAD0-9` 与四则运算、小数点、分隔符各有独立虚拟键码，直接映射；
        //   * 导航区与小键盘**共用 VK**，来处只在 lParam 的扫描码里，而六个键里只有 `Home`
        //     真的可分（见上）。故只有 `VK_HOME` 走双分支，其余恒给主键码、维持现网行为。
        case VK_NUMPAD0:
            return KeyCode::KP_0;
        case VK_NUMPAD1:
            return KeyCode::KP_1;
        case VK_NUMPAD2:
            return KeyCode::KP_2;
        case VK_NUMPAD3:
            return KeyCode::KP_3;
        case VK_NUMPAD4:
            return KeyCode::KP_4;
        case VK_NUMPAD5:
            // NumLock 关闭时小键盘 5 是 Begin 位。框架不做二次翻译（见 keycode.h 口径），
            // 恒给 KP_5，由消费方按 NumLock 位自行决定要不要当 Begin 用。
            return KeyCode::KP_5;
        case VK_NUMPAD6:
            return KeyCode::KP_6;
        case VK_NUMPAD7:
            return KeyCode::KP_7;
        case VK_NUMPAD8:
            return KeyCode::KP_8;
        case VK_NUMPAD9:
            return KeyCode::KP_9;
        case VK_ADD:
            return KeyCode::KP_Add;
        case VK_SUBTRACT:
            return KeyCode::KP_Subtract;
        case VK_MULTIPLY:
            return KeyCode::KP_Multiply;
        case VK_DIVIDE:
            return KeyCode::KP_Divide;
        case VK_DECIMAL:
            return KeyCode::KP_Decimal;
        case VK_SEPARATOR:
            return KeyCode::KP_Separator;
        default:
            if (vk >= VK_F1 && vk <= VK_F12) {
                return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + (vk - VK_F1));
            }
            return KeyCode::Unknown;
    }
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
