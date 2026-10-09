#pragma once

/// @brief 平台无关逻辑键码模块：`KeyCode` 枚举与键名查询 `key_name`、反查 `key_code_from_name`。
/// @file keycode.h

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace aurora {

/// @brief 平台无关的逻辑键码（specification/05-event-navigation.md §2.2）。
///
/// widget/事件层只认 `KeyCode`，不依赖任何平台键值。具体平台（如 GLFW）在各自的
/// 后端中把原生键码翻译成 `KeyCode`（见 `window/glfw_surface.h` 的 `fromGlfwKey`），
/// 从而保持 `event` 模块平台无关。新增键位时在此追加枚举值即可。
/// 语义为「物理键位 + 功能键」：A–Z / D0–9 不区分大小写与 shift 态（大小写由文本
/// 输入事件区分）；Shift/Control/Alt/Meta 将左右两侧合并为单一逻辑键；
/// 标点键按美式（US）物理布局命名；数字小键盘单列 `KP_*` 键码（见枚举末尾）；
/// 未能识别的原生键一律落到 `Unknown`。
///
/// **数字小键盘（Keypad）口径**：
///   * `KP_*` 全集已建模（含 0-9、导航六键、四则运算、小数点与分隔符）；
///   * `NumLock` **只建模为修饰位**（`ModifierKey::NumLock`，`event.h`），不建模为键码——它本身
///     是切换键、对终端无发送意义，而消费方真正需要的是「本条按键发生时 NumLock 是开还是关」
///     这一位；
///   * **框架不做二次翻译**：`KP_Prior` 恒为 `KP_Prior`，不会因 NumLock 关闭就降级成 `PageUp`。
///     「NumLock 关闭时 `KP_Prior` 语义 = PageUp」这类降级由消费方按
///     `modifiers & ModifierKey::NumLock` 自行决定，宿主只负责「平台原始键 → 语义键码 + 修饰态」
///     的一一映射。宿主侧读不到该位时按「关」处理，不会静默假报「开」。
///   * 小键盘回车（`KP_Enter`）按既有决定并入 `KeyCode::Enter`，不单列。
///   * **跨后端产出的键码集不完全一致，这是平台事实而非疏漏**：
///     - `KP_0-9` 与运算键（`KP_Add` / `KP_Subtract` / `KP_Multiply` / `KP_Divide` /
///       `KP_Decimal` / `KP_Separator`）三后端齐备；
///     - `KP_Insert` / `KP_Delete` / `KP_Begin` / `KP_End` / `KP_Home` / `KP_Prior` /
///       `KP_Next` **只有 X11 / Wayland 后端产出**（X11 的 KP_* keysym 与主键各占独立码点，
///       天然可分）。Win32 在导航区把两边**共用同一组虚拟键码**，实测六个键里只有 `Home`
///       真的可分（主键盘扫描码 `0x47` / 小键盘 `E0 4E`），其余五个键两边的
///       (VK, scan, extended) 完全相同或仅 extended 位不同，无法可靠判别；GLFW 的键码表
///       本身就没有 `GLFW_KEY_KP_INSERT` / `_HOME` 之类的常量。**故消费方在 Win32 与 GLFW
///       上不能假定这几项一定命中**，需要时应按平台兜底。
///   * **主键盘 `Insert`（`Insert = 123`）与 `KP_Insert` 并存是刻意的**，两档不可互换：
///     X11 / Wayland 能区分来处（两个 keysym 各占独立码点）就区分；Win32 / GLFW 区分不了就
///     **恒给主档 `Insert`**——Win32 两区共用 `VK_INSERT`（判据见 `win32_modifiers.h` 的
///     `is_numpad_nav_scan` 对照表），GLFW 键码表没有 `GLFW_KEY_KP_INSERT` 常量。
///     **「恒给主档」不是 bug**：两区的终端语义本就等价（同为 CSI 2~），而按扫描码二次判定会把
///     Win32 拖进一条与其他后端不可对齐的私有口径。后来者不要把它「修」成按来处分派。
///   * 新增键位一律**追加到枚举末尾并写死显式数值**，理由见末尾 KP_* 段的注释。
///   * 本枚举**刻意**混用隐式与显式初值：既有段（0..94）靠隐式连号，新增的 KP_* 段首项
///     写死 100 作「本段只能追加」的机器可读锚点。全量显式化会把 0..94 逐个数字钉进源码、
///     与「分组连续」的既有契约（`utest_keycode` 的分组断言）重复，且失去「改动中间值时
///     编译器会替你重排后续项」这一保护。
enum class KeyCode : int {  // NOLINT(*-enum-size, readability-enum-initial-value)
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
    D0,  ///< 主键盘数字行 0（数字小键盘的对应键建模为 `KP_0`，见枚举末尾的 KP_* 段）。
    D1,  ///< 主键盘数字行 1（数字小键盘的对应键建模为 `KP_1`）。
    D2,  ///< 主键盘数字行 2（数字小键盘的对应键建模为 `KP_2`）。
    D3,  ///< 主键盘数字行 3（数字小键盘的对应键建模为 `KP_3`）。
    D4,  ///< 主键盘数字行 4（数字小键盘的对应键建模为 `KP_4`）。
    D5,  ///< 主键盘数字行 5（数字小键盘的对应键建模为 `KP_5`）。
    D6,  ///< 主键盘数字行 6（数字小键盘的对应键建模为 `KP_6`）。
    D7,  ///< 主键盘数字行 7（数字小键盘的对应键建模为 `KP_7`）。
    D8,  ///< 主键盘数字行 8（数字小键盘的对应键建模为 `KP_8`）。
    D9,  ///< 主键盘数字行 9（数字小键盘的对应键建模为 `KP_9`）。

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

    // ---- 数字小键盘（Keypad）----
    //
    // 追加在枚举末尾并**写死显式数值**：消费方普遍持有「键码 → 平台原生值」的逐值对齐映射表，
    // 若把新项插进字母 / 数字行之间，那些表会**静默错位**（不报错、只是全盘对不上）。
    // `KP_Insert = 100` 起顺序固定，新增只许往后接。
    //
    // **框架不做二次翻译**：`KP_Prior`（小键盘 PageUp）在本枚举里恒为 `KP_Prior`，不会因 NumLock
    // 关闭就降级成 `PageUp`——那层语义归消费方按 `modifiers & ModifierKey::NumLock` 自行决定。
    // 宿主的职责只到这里：「平台原始键 → 语义键码 + 修饰态」的一一映射。
    //
    // 首项写死 100 的理由见枚举声明上方：它是「本段只能追加、不得插在中间」这条纪律的
    // 机器可读锚点。取值 100 与既有段（0..94）留出明确间隔，间隔本身不承载语义、
    // 只承载「这段是后加的」这一事实。
    KP_Insert = 100,  ///< 小键盘 Insert（导航区；语义等价 Insert，非主键盘 `Delete`）。
    KP_Delete,  ///< 小键盘 Delete（导航区）。
    KP_Begin,  ///< 小键盘 5 无 NumLock 时的 Home 键位（仅部分布局存在此物理位）。
    KP_End,  ///< 小键盘 End 键位。
    KP_Home,  ///< 小键盘 Home 键位。
    KP_Prior,  ///< 小键盘 PageUp 键位（NumLock 关闭时消费方通常当光标上移用）。
    KP_Next,  ///< 小键盘 PageDown 键位（NumLock 关闭时消费方通常当光标下移用）。
    KP_Add,  ///< 小键盘加号键（NumLock 关闭时部分布局给导航语义）。
    KP_Subtract,  ///< 小键盘减号键。
    KP_Multiply,  ///< 小键盘乘号键。
    KP_Divide,  ///< 小键盘除号键（部分布局兼作此键）。
    KP_Decimal,  ///< 小键盘小数点键（NumLock 关闭时部分布局给 Delete 语义）。
    KP_Separator,  ///< 小键盘分隔符键（部分布局的次级分隔位）。
    KP_0,  ///< 小键盘数字 0（NumLock 关闭时消费方通常映射为 `KP_Insert` 语义）。
    KP_1,  ///< 小键盘数字 1（NumLock 关闭时消费方通常映射为 `KP_End`）。
    KP_2,  ///< 小键盘数字 2（NumLock 关闭时消费方通常映射为 `KP_Down`）。
    KP_3,  ///< 小键盘数字 3（NumLock 关闭时消费方通常映射为 `KP_Next`）。
    KP_4,  ///< 小键盘数字 4（NumLock 关闭时消费方通常映射为 `KP_Left`）。
    KP_5,  ///< 小键盘数字 5（NumLock 关闭时消费方通常映射为 `KP_Begin`）。
    KP_6,  ///< 小键盘数字 6（NumLock 关闭时消费方通常映射为 `KP_Right`）。
    KP_7,  ///< 小键盘数字 7（NumLock 关闭时消费方通常映射为 `KP_Home`）。
    KP_8,  ///< 小键盘数字 8（NumLock 关闭时消费方通常映射为 `KP_Up`）。
    KP_9,  ///< 小键盘数字 9（NumLock 关闭时消费方通常映射为 `KP_Prior`）。

    // ---- 主键盘 Insert（后补段）----
    //
    // 语义上属于「编辑 / 导航」段（`Home` / `End` / `PageUp` 那一组），却**不**插回该段：
    // 既有段（0..94）靠隐式连号，往中间插一项会让它之后的全部取值整体位移，而消费方普遍
    // 持有「键码 → 平台原生值」的逐值对齐映射表——位移不报错、只会全盘静默错位。故本键取
    // `KP_9`（122）之后的下一个显式初值位，与 `KP_Insert = 100` 同一条「只许往后接」的纪律。
    //
    // 它与 `KP_Insert` 的关系（两档并存、Win32 / GLFW 恒给本档）见文件头的口径注释。
    Insert = 123,  ///< 主键盘 Insert（导航区；NumLock 关闭时小键盘 0 也发同一个平台键）。
};

namespace detail {

/// @brief 键名字面量的唯一真源：逐键码的 switch。
/// @param [in] k 待查询的逻辑键码。
/// @return 键名静态字符串字面量（如 "Enter"、"ArrowLeft"）；枚举未覆盖的取值返回 "Unknown"。
/// @note 本函数是键名的唯一来源：公共 `key_name` 所查的键名表由它**在编译期逐槽展开**得到
///       （见 `AURORA_KEY_NAMES`），二者物理上不可能漂移。保留 switch 形态而非手写数组表，
///       是为了留住 `-Wswitch`——新增枚举值而漏写分支时编译期即报错，手写数组表没有这层保护。
[[nodiscard]] constexpr auto key_name_switch(KeyCode k) -> const char * {
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
        case KeyCode::Insert:
            return "Insert";
        case KeyCode::KP_Insert:
            return "KP_Insert";
        case KeyCode::KP_Delete:
            return "KP_Delete";
        case KeyCode::KP_Begin:
            return "KP_Begin";
        case KeyCode::KP_End:
            return "KP_End";
        case KeyCode::KP_Home:
            return "KP_Home";
        case KeyCode::KP_Prior:
            return "KP_Prior";
        case KeyCode::KP_Next:
            return "KP_Next";
        case KeyCode::KP_Add:
            return "KP_Add";
        case KeyCode::KP_Subtract:
            return "KP_Subtract";
        case KeyCode::KP_Multiply:
            return "KP_Multiply";
        case KeyCode::KP_Divide:
            return "KP_Divide";
        case KeyCode::KP_Decimal:
            return "KP_Decimal";
        case KeyCode::KP_Separator:
            return "KP_Separator";
        case KeyCode::KP_0:
            return "KP_0";
        case KeyCode::KP_1:
            return "KP_1";
        case KeyCode::KP_2:
            return "KP_2";
        case KeyCode::KP_3:
            return "KP_3";
        case KeyCode::KP_4:
            return "KP_4";
        case KeyCode::KP_5:
            return "KP_5";
        case KeyCode::KP_6:
            return "KP_6";
        case KeyCode::KP_7:
            return "KP_7";
        case KeyCode::KP_8:
            return "KP_8";
        case KeyCode::KP_9:
            return "KP_9";
    }
    return "Unknown";
}

/// @brief 键名表的槽位数（键码索引空间 `[0, N)` 的宽度）。
/// @note 取 256 而非「当前最大显式值 + 1」（`Insert` 的 123 + 1）是刻意的：新增键位只改
///       `key_name_switch` 一处的 switch，**本槽位数无需同步**，表内容在编译期由该 switch 逐槽
///       展开（见 `AURORA_KEY_NAMES`），落在空洞与越界的槽位一并落到 `"Unknown"`。槽位数写成
///       紧贴最大枚举值会把「加键忘了扩容」变成静默故障：新键码查表越界、恒返回 `"Unknown"`，
///       且没有任何编译期信号。
inline constexpr int AURORA_KEY_NAME_TABLE_SIZE = 256;

/// @brief 把 `key_name_switch` 在编译期逐槽展开成键名表。
/// @tparam I 槽位下标序列（由 `std::make_integer_sequence` 提供）。
/// @return 槽位 i 恒为 `key_name_switch(static_cast<KeyCode>(i))` 的键名表。
template <int... I>
[[nodiscard]] constexpr auto build_key_name_table([[maybe_unused]] std::integer_sequence<int, I...> slots)
    -> std::array<const char *, sizeof...(I)> {
    return std::array<const char *, sizeof...(I)>{{key_name_switch(static_cast<KeyCode>(I))...}};
}

/// @brief 键名表：槽位 i 恒等于 `key_name_switch(static_cast<KeyCode>(i))`。
/// @note `inline` 是必须的：只写 `constexpr`（内部链接）会让**每个**包含本头的 TU 各持一份表
///       （实测 20 个 TU = 20 份表符号、19.8 KB `.data.rel.ro`）；`inline constexpr` 使它在
///       comdat 中合并为全程序一份。
inline constexpr auto AURORA_KEY_NAMES =
    build_key_name_table(std::make_integer_sequence<int, AURORA_KEY_NAME_TABLE_SIZE>{});

}  // namespace detail

/// @brief 返回键码的可读名称（用于调试/日志）。
/// @param [in] k 待查询的逻辑键码。
/// @return 键名静态字符串字面量（如 "Enter"、"ArrowLeft"）；枚举未覆盖的取值返回 "Unknown"。
/// @note 实现是「键名表 + 一次索引」，单次查询成本与键码取值、编译器、优化档**均无关**。写成
///       switch 时其 O(1) 依赖编译器是否生成跳转表——实测 GCC -O0 退化成比较链，查 `A` 与查
///       `Insert` 的耗时相差 8 倍；本形态在 -O0 下仍是恒定索引。
[[nodiscard]] inline auto key_name(KeyCode k) -> const char * {
    const auto index = static_cast<int>(k);
    if (index < 0 || index >= detail::AURORA_KEY_NAME_TABLE_SIZE) {
        return "Unknown";
    }
    // 下标已由上方区间检查钉死在 `[0, 表长)`；这里刻意用不做二次检查的 operator[]（而非 at()），
    // 以免为一条已证明的不变式再付一次分支。
    // （理由写在指令**之前**：写在同行会被 clang-format 在 120 列折行，指令便只覆盖到注释。）
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return detail::AURORA_KEY_NAMES[static_cast<std::size_t>(index)];
}

/// @brief 键名 → 键码的反查（`key_name` 的逆，specification/05-event-navigation.md §2.2）。
///
/// **反查不另列键名表**：遍历键名表、逐槽取内容与入参比对，故键名字面量的唯一来源仍是
/// `detail::key_name_switch` 那一处——新增键位只改它，不存在第二张表可与之漂移。
/// 「另抄一份反查表」是本函数刻意规避的形态：那张表与真源同源却分叉，且框架侧无任何
/// 门禁能发现两边的漂移。排除判据是**按输出串排除**（跳过 `"Unknown"`）而非按键码排除：
/// 越界键码的 `key_name` 同样是 `"Unknown"`，只挡 `KeyCode::Unknown` 挡不住它们。
///
/// @param [in] name 待反查的键名；须与 `key_name` 的输出**逐字节一致**（区分大小写；非 ASCII 恒不命中）。
/// @return 命中为对应键码；未命中为 `std::nullopt`。占位键名 `"Unknown"` **不予接受**——它是
///         「后端映射表未收录」的占位值而非可绑定的键位，认下它等于把「解析不出来」假报成
///         「绑定到了某个键」，与「解析失败不得回落假有效值」同口径。
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
[[nodiscard]] inline auto key_code_from_name(std::string_view name) -> std::optional<KeyCode> {
    for (int raw = 0; raw < detail::AURORA_KEY_NAME_TABLE_SIZE; ++raw) {
        // 取值在底层类型 int 内；枚举空洞与越界取值由下方「跳过 "Unknown"」那一支排除。
        // （理由写在指令**之前**：写在同行会被 clang-format 在 120 列折行，指令便只覆盖到注释。）
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        const auto code = static_cast<KeyCode>(raw);
        const std::string_view candidate{key_name(code)};
        if (candidate == "Unknown") {
            continue;  // 兜底串（含枚举空洞与越界取值），不是键名
        }
        if (name == candidate) {
            return code;
        }
    }
    return std::nullopt;
}

}  // namespace aurora
