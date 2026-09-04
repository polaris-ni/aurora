#pragma once

namespace aurora {

/**
 * @brief 平台无关的逻辑键码（specification/05-event-navigation.md §2.2）。
 *
 * widget/事件层只认 `KeyCode`，不依赖任何平台键值。具体平台（如 GLFW）在各自的
 * 后端中把原生键码翻译成 `KeyCode`（见 `window/glfw_surface.h` 的 `fromGlfwKey`），
 * 从而保持 `event` 模块平台无关。新增键位时在此追加枚举值即可。
 */
enum class KeyCode : int {  // NOLINT(*-enum-size)
    Unknown = 0,

    // 字母 A-Z
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // 数字 0-9
    D0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    D8,
    D9,

    // 功能键
    Escape,
    Enter,
    Tab,
    Backspace,
    Delete,
    Space,

    // 方向键
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,

    // 修饰键
    Shift,
    Control,
    Alt,
    Meta,

    // 编辑/导航
    Home,
    End,
    PageUp,
    PageDown,

    // 标点（美式布局）
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Quote,
    Comma,
    Period,
    Slash,
    Backquote,

    // 功能键 F1-F12
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

/// @brief 返回键码的可读名称（用于调试/日志）；未知键返回 "Unknown"。
[[nodiscard]] inline auto key_name(KeyCode k) -> const char * {
    switch (k) {
        case KeyCode::Unknown:
            return "Unknown";
        case KeyCode::A:
            return "A";
        case KeyCode::B:
            return "B";
        case KeyCode::C:
            return "C";
        case KeyCode::D:
            return "D";
        case KeyCode::E:
            return "E";
        case KeyCode::F:
            return "F";
        case KeyCode::G:
            return "G";
        case KeyCode::H:
            return "H";
        case KeyCode::I:
            return "I";
        case KeyCode::J:
            return "J";
        case KeyCode::K:
            return "K";
        case KeyCode::L:
            return "L";
        case KeyCode::M:
            return "M";
        case KeyCode::N:
            return "N";
        case KeyCode::O:
            return "O";
        case KeyCode::P:
            return "P";
        case KeyCode::Q:
            return "Q";
        case KeyCode::R:
            return "R";
        case KeyCode::S:
            return "S";
        case KeyCode::T:
            return "T";
        case KeyCode::U:
            return "U";
        case KeyCode::V:
            return "V";
        case KeyCode::W:
            return "W";
        case KeyCode::X:
            return "X";
        case KeyCode::Y:
            return "Y";
        case KeyCode::Z:
            return "Z";
        case KeyCode::D0:
            return "0";
        case KeyCode::D1:
            return "1";
        case KeyCode::D2:
            return "2";
        case KeyCode::D3:
            return "3";
        case KeyCode::D4:
            return "4";
        case KeyCode::D5:
            return "5";
        case KeyCode::D6:
            return "6";
        case KeyCode::D7:
            return "7";
        case KeyCode::D8:
            return "8";
        case KeyCode::D9:
            return "9";
        case KeyCode::Escape:
            return "Escape";
        case KeyCode::Enter:
            return "Enter";
        case KeyCode::Tab:
            return "Tab";
        case KeyCode::Backspace:
            return "Backspace";
        case KeyCode::Delete:
            return "Delete";
        case KeyCode::Space:
            return "Space";
        case KeyCode::ArrowLeft:
            return "ArrowLeft";
        case KeyCode::ArrowRight:
            return "ArrowRight";
        case KeyCode::ArrowUp:
            return "ArrowUp";
        case KeyCode::ArrowDown:
            return "ArrowDown";
        case KeyCode::Shift:
            return "Shift";
        case KeyCode::Control:
            return "Control";
        case KeyCode::Alt:
            return "Alt";
        case KeyCode::Meta:
            return "Meta";
        case KeyCode::Home:
            return "Home";
        case KeyCode::End:
            return "End";
        case KeyCode::PageUp:
            return "PageUp";
        case KeyCode::PageDown:
            return "PageDown";
        case KeyCode::Minus:
            return "Minus";
        case KeyCode::Equal:
            return "Equal";
        case KeyCode::LeftBracket:
            return "LeftBracket";
        case KeyCode::RightBracket:
            return "RightBracket";
        case KeyCode::Backslash:
            return "Backslash";
        case KeyCode::Semicolon:
            return "Semicolon";
        case KeyCode::Quote:
            return "Quote";
        case KeyCode::Comma:
            return "Comma";
        case KeyCode::Period:
            return "Period";
        case KeyCode::Slash:
            return "Slash";
        case KeyCode::Backquote:
            return "Backquote";
        case KeyCode::F1:
            return "F1";
        case KeyCode::F2:
            return "F2";
        case KeyCode::F3:
            return "F3";
        case KeyCode::F4:
            return "F4";
        case KeyCode::F5:
            return "F5";
        case KeyCode::F6:
            return "F6";
        case KeyCode::F7:
            return "F7";
        case KeyCode::F8:
            return "F8";
        case KeyCode::F9:
            return "F9";
        case KeyCode::F10:
            return "F10";
        case KeyCode::F11:
            return "F11";
        case KeyCode::F12:
            return "F12";
    }
    return "Unknown";
}

}  // namespace aurora
