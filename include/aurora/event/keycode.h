#pragma once

/// @brief 平台无关逻辑键码模块：`KeyCode` 枚举与键名查询 `key_name`。
/// @file keycode.h

namespace aurora {

/// @brief 平台无关的逻辑键码（specification/05-event-navigation.md §2.2）。
///
/// widget/事件层只认 `KeyCode`，不依赖任何平台键值。具体平台（如 GLFW）在各自的
/// 后端中把原生键码翻译成 `KeyCode`（见 `window/glfw_surface.h` 的 `fromGlfwKey`），
/// 从而保持 `event` 模块平台无关。新增键位时在此追加枚举值即可。
/// 语义为「物理键位 + 功能键」：A–Z / D0–9 不区分大小写与 shift 态（大小写由文本
/// 输入事件区分）；Shift/Control/Alt/Meta 将左右两侧合并为单一逻辑键；
/// 标点键按美式（US）物理布局命名；未能识别的原生键一律落到 `Unknown`。
enum class KeyCode : int {  // NOLINT(*-enum-size)
    Unknown = 0,  ///< 未映射键占位值：后端映射表未收录的原生键（如 GLFW/X11 映射的 default 分支）给出此项。

    // 字母 A-Z
    A,  ///< 字母键 A（物理键位，大小写共用同一键码）。
    B,  ///< 字母键 B（物理键位，大小写共用同一键码）。
    C,  ///< 字母键 C（物理键位，大小写共用同一键码）。
    D,  ///< 字母键 D（物理键位，大小写共用同一键码）。
    E,  ///< 字母键 E（物理键位，大小写共用同一键码）。
    F,  ///< 字母键 F（物理键位，大小写共用同一键码）。
    G,  ///< 字母键 G（物理键位，大小写共用同一键码）。
    H,  ///< 字母键 H（物理键位，大小写共用同一键码）。
    I,  ///< 字母键 I（物理键位，大小写共用同一键码）。
    J,  ///< 字母键 J（物理键位，大小写共用同一键码）。
    K,  ///< 字母键 K（物理键位，大小写共用同一键码）。
    L,  ///< 字母键 L（物理键位，大小写共用同一键码）。
    M,  ///< 字母键 M（物理键位，大小写共用同一键码）。
    N,  ///< 字母键 N（物理键位，大小写共用同一键码）。
    O,  ///< 字母键 O（物理键位，大小写共用同一键码）。
    P,  ///< 字母键 P（物理键位，大小写共用同一键码）。
    Q,  ///< 字母键 Q（物理键位，大小写共用同一键码）。
    R,  ///< 字母键 R（物理键位，大小写共用同一键码）。
    S,  ///< 字母键 S（物理键位，大小写共用同一键码）。
    T,  ///< 字母键 T（物理键位，大小写共用同一键码）。
    U,  ///< 字母键 U（物理键位，大小写共用同一键码）。
    V,  ///< 字母键 V（物理键位，大小写共用同一键码）。
    W,  ///< 字母键 W（物理键位，大小写共用同一键码）。
    X,  ///< 字母键 X（物理键位，大小写共用同一键码）。
    Y,  ///< 字母键 Y（物理键位，大小写共用同一键码）。
    Z,  ///< 字母键 Z（物理键位，大小写共用同一键码）。

    // 数字 0-9
    D0,  ///< 主键盘数字行 0（数字小键盘的 KP_0-9 键未建模，映射到 Unknown）。
    D1,  ///< 主键盘数字行 1（数字小键盘的对应键未建模）。
    D2,  ///< 主键盘数字行 2（数字小键盘的对应键未建模）。
    D3,  ///< 主键盘数字行 3（数字小键盘的对应键未建模）。
    D4,  ///< 主键盘数字行 4（数字小键盘的对应键未建模）。
    D5,  ///< 主键盘数字行 5（数字小键盘的对应键未建模）。
    D6,  ///< 主键盘数字行 6（数字小键盘的对应键未建模）。
    D7,  ///< 主键盘数字行 7（数字小键盘的对应键未建模）。
    D8,  ///< 主键盘数字行 8（数字小键盘的对应键未建模）。
    D9,  ///< 主键盘数字行 9（数字小键盘的对应键未建模）。

    // 功能键
    Escape,  ///< Esc 取消/退出键。
    Enter,  ///< 回车键；X11/Wayland 侧 KP_Enter 并入本键，Win32 小键盘回车同发 VK_RETURN（GLFW 仅映射主回车）。
    Tab,  ///< Tab 制表键（焦点移动的主要物理来源）。
    Backspace,  ///< 退格键（删除光标前字符）。
    Delete,  ///< 删除键（删除光标后字符，向前删除）。
    Space,  ///< 空格键。

    // 方向键
    ArrowLeft,  ///< 左方向键。
    ArrowRight,  ///< 右方向键。
    ArrowUp,  ///< 上方向键。
    ArrowDown,  ///< 下方向键。

    // 修饰键
    Shift,  ///< Shift 修饰键（左右两侧合并，不区分左/右 Shift）。
    Control,  ///< Ctrl 修饰键（左右两侧合并）。
    Alt,  ///< Alt 修饰键（左右两侧合并；Linux 侧 Super_L/Super_R 归 Meta 而非本键）。
    Meta,  ///< 系统修饰键：Windows 键、Linux Super、macOS Cmd（左右两侧合并）。

    // 编辑/导航
    Home,  ///< Home 行首/文首导航键。
    End,  ///< End 行尾/文末导航键。
    PageUp,  ///< PageUp 上翻页键（X11 keysym 名为 Prior）。
    PageDown,  ///< PageDown 下翻页键（X11 keysym 名为 Next）。

    // 标点（美式布局）
    Minus,  ///< 减号/连字符键 -。
    Equal,  ///< 等号键 =。
    LeftBracket,  ///< 左方括号键 [。
    RightBracket,  ///< 右方括号键 ]。
    Backslash,  ///< 反斜杠键 \。
    Semicolon,  ///< 分号键 ;。
    Quote,  ///< 单引号键 '（美式布局回车右侧，GLFW 称 APOSTROPHE）。
    Comma,  ///< 逗号键 ,。
    Period,  ///< 句号键 .。
    Slash,  ///< 斜杠键 /。
    Backquote,  ///< 反引号键 `（美式布局主数字行左上方，GLFW 称 GRAVE_ACCENT）。

    // 功能键 F1-F12
    F1,  ///< 顶部功能键 F1。
    F2,  ///< 顶部功能键 F2。
    F3,  ///< 顶部功能键 F3。
    F4,  ///< 顶部功能键 F4。
    F5,  ///< 顶部功能键 F5。
    F6,  ///< 顶部功能键 F6。
    F7,  ///< 顶部功能键 F7。
    F8,  ///< 顶部功能键 F8。
    F9,  ///< 顶部功能键 F9。
    F10,  ///< 顶部功能键 F10。
    F11,  ///< 顶部功能键 F11。
    F12,  ///< 顶部功能键 F12。
};

/// @brief 返回键码的可读名称（用于调试/日志）。
/// @param [in] k 待查询的逻辑键码。
/// @return 键名静态字符串字面量（如 "Enter"、"ArrowLeft"）；枚举未覆盖的取值返回 "Unknown"。
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
