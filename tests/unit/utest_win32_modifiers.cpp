/// @file utest_win32_modifiers.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/win32_modifiers.h
/// 测试说明: Win32 宿主修饰键跟踪器 —— 位翻转随消息推进、左右与通用码归并、失焦清空、激活
///           播种、AltGr 两段序列。它取代了原先「派发时刻 `GetAsyncKeyState` 采样」，故这些
///           消息序列正是热键能否命中的判据本体（竞态实测见 `manual-test/21-debug.md` TC-DEBUG-004）
///           另覆盖 `WM_SYSKEY*` 的派发分叉判据 `syskey_dispatches()`：除 Alt 自身与 F10 外
///           一律进派发链，而例外项仍照常推进修饰态
///           锁定态位（NumLock）单列一组：它按 toggle 记账（Down 翻转 / Up 不翻）且与按住态
///           分离成两份状态——`bit_for` 刻意不认它，由 `lock_bit_for` 承载；`clear()` 只清
///           按住态（锁定态没有抬起消息可言，抹成「关」是假报）
/// 平台门控: 依赖 `<windows.h>` 的 VK 码与后端宏；宏未开启时每条用例落 SKIP 桩（声明无条件可见）

#include <cstdint>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))
// 平台可用性开关，供下方各用例体内的 #ifdef 使用，无 constexpr 等价物
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_WIN32_MODIFIERS_AVAILABLE 1
#endif

#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
#include <windows.h>

#include "aurora/event/event.h"
#include "aurora/window/detail/win32_keymap.h"
#include "aurora/window/detail/win32_modifiers.h"
#endif

namespace aurora::test_cases::utest_win32_modifiers {

#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
using detail::ModifierKeyTracker;

/// @brief 断言辅助：把 `ModifierKey` 位集与期望逐位比对（枚举位掩码按 uint8_t 比）。
auto check_mods(const ModifierKeyTracker &t, ModifierKey want) -> void {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(t.get()), static_cast<std::uint8_t>(want));
}
#endif

#define AURORA_WIN32_MODIFIERS_SKIP \
    AURORA_TEST_SKIP("non-Windows or Win32/D3D11 backend off: tracker needs VK_* codes from <windows.h>")

AURORA_TEST_CASE(down_sets_bit_and_up_clears_it) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, true));
    check_mods(t, ModifierKey::Control);
    // 自动重复（按住不放连发 WM_KEYDOWN）不得叠出第二个状态
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, true));
    check_mods(t, ModifierKey::Control);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(left_right_and_generic_vk_share_one_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 归并到同一位的收益：按下与抬起给出**不同变体**（注入与某些布局会这样）也不残留幻影。
    ModifierKeyTracker t;
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LSHIFT, true));
    check_mods(t, ModifierKey::Shift);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_SHIFT, false));  // 抬起给的是通用码，不是 L 变体
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RSHIFT, true));
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LSHIFT, false));  // 抬起给另一侧变体
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(chord_carries_the_modifier_still_down_in_the_queue) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 被修掉的回归本体：`Ctrl+S` 里 Ctrl 的抬起消息排在 S 之后，则无论泵送多晚，派发 S 时
    // 跟踪器都还带着 Control。物理采样做不到这点——抬起一旦落地读数即翻 0。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    AURORA_TEST_CHECK_FALSE(t.apply(0x53 /* 'S' */, true));  // 非修饰键：不改状态、回 false
    check_mods(t, ModifierKey::Control);
    t.apply(0x53, false);
    t.apply(VK_CONTROL, false);
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(altgr_two_stage_sequence_yields_alt_and_control) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // AltGr 在 Windows 上是 `VK_RMENU` + `VK_LCONTROL` 两段（抬起反序），逐位翻转天然覆盖。
    ModifierKeyTracker t;
    t.apply(VK_LCONTROL, true);
    t.apply(VK_RMENU, true);
    check_mods(t, ModifierKey::Control | ModifierKey::Alt);
    t.apply(VK_LCONTROL, false);
    check_mods(t, ModifierKey::Alt);
    t.apply(VK_RMENU, false);
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(blur_clears_every_held_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 交出前台/焦点后的抬起消息属于别的窗口，跟踪器再也收不到 → 必须整体归零。
    ModifierKeyTracker t;
    t.apply(VK_LMENU, true);
    t.apply(VK_SHIFT, true);
    check_mods(t, ModifierKey::Alt | ModifierKey::Shift);
    t.clear();
    check_mods(t, ModifierKey::None);
    // 清空后真正的抬起落地（切回来时系统补发）也不该把它「抬」回来
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LMENU, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(seed_replaces_state_wholesale) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 激活基线播种：Alt+Tab 切进来时 Alt 已按下，那条按下属于旧前台窗口。
    // 注意 seed 按掩码**拆分**写入：这条读数里没有锁定位，故 locks_ 落 None。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    t.clear();  // 交出前台
    t.seed(ModifierKey::Alt);
    check_mods(t, ModifierKey::Alt);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_MENU, false));  // 用户随后松开 Alt
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(meta_maps_both_win_keys) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LWIN, true));
    check_mods(t, ModifierKey::Meta);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LWIN, false));
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RWIN, true));
    check_mods(t, ModifierKey::Meta);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RWIN, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(non_modifier_keys_never_touch_the_state) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    for (const int vk : {VK_TAB, VK_LEFT, 0x41 /* 'A' */, VK_SPACE, VK_ESCAPE}) {
        AURORA_TEST_CHECK_FALSE(t.apply(vk, true));
        AURORA_TEST_CHECK_FALSE(t.apply(vk, false));
        check_mods(t, ModifierKey::Control);
    }
    // 大写锁定既不是修饰位也不该污染状态（`VK_CAPITAL` 不在映射内）
    AURORA_TEST_CHECK_FALSE(t.apply(VK_CAPITAL, true));
    check_mods(t, ModifierKey::Control);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(syskey_dispatches_all_but_alt_itself_and_f10) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // `WM_SYSKEY*` 的分叉判据本体。改动这张表 = 改「哪些 Alt 组合能被 Aurora 看见」。
    // 例外只有两类：Alt 自身（左右都算，纯修饰语义）与 F10（系统菜单键）。
    for (const int vk : {VK_MENU, VK_LMENU, VK_RMENU, VK_F10}) {
        AURORA_TEST_CHECK_FALSE(detail::syskey_dispatches(vk));
    }
    // 其余一律进派发：字母 / 功能键 / 编辑键 / 小键盘（含 Alt 组合的系统键如 Alt+Enter）。
    for (const int vk : {0x41 /* 'A' */, 0x7A /* 'Z' */, VK_F4, VK_RETURN, VK_TAB, VK_ESCAPE, VK_LEFT, VK_DELETE,
                         VK_SPACE, VK_NUMPAD0, VK_ADD}) {
        AURORA_TEST_CHECK_TRUE(detail::syskey_dispatches(vk));
    }
    // 判据是纯函数（不读状态）：同一 vk 反复判定结果一致，与跟踪器当前位集无关。
    ModifierKeyTracker t;
    t.apply(VK_LMENU, true);
    AURORA_TEST_CHECK_FALSE(detail::syskey_dispatches(VK_LMENU));
    t.clear();
    AURORA_TEST_CHECK_FALSE(detail::syskey_dispatches(VK_LMENU));
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(syskey_exceptions_still_advance_the_modifier_state) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 「只推进态、不派发」不能读成「不处理」：Alt 自身的 syskey 若不推进修饰位，
    // 紧随其后的 Alt+字母就会错报不带 Alt（这正是引入 dispatch 分叉前的原始动机）。
    ModifierKeyTracker t;
    AURORA_TEST_CHECK_FALSE(detail::syskey_dispatches(VK_LMENU));
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LMENU, true));
    check_mods(t, ModifierKey::Alt);
    // 随后的 Alt+字母进派发，且该位带着 Alt —— 这就是「修饰态推进必须早于派发」的可测形态。
    AURORA_TEST_CHECK_TRUE(detail::syskey_dispatches(0x41 /* 'A' */));
    AURORA_TEST_CHECK_FALSE(t.apply(0x41, true));
    check_mods(t, ModifierKey::Alt);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LMENU, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
/// @brief 构造一条带指定扫描码与 extended 位的 `WM_KEYDOWN` lParam。
/// @param scan 扫描码字节。
/// @param extended 是否带 E0 前缀（extended 位）。
/// @return 拼好的 lParam。
constexpr auto make_keydown_lparam(unsigned char scan, bool extended) -> LPARAM {
    return static_cast<LPARAM>(1ULL | (static_cast<unsigned long long>(scan) << 16U) |
                               (static_cast<unsigned long long>(extended ? 1U : 0U) << 24U) | (1ULL << 30U) |
                               (1ULL << 31U));
}
#endif

// 指针事件（MouseEvent / ScrollEvent）现在也消费 tracker 的 `get()` 值——Win32 后端在
// `on_mouse` / `on_wheel` 里把它盖进事件字段（specification/05-event-navigation.md §2.2.2）。
// 这给跟踪器加了一条**此前不存在的耦合**：修饰态的准确性从此不再只影响键盘热键，也决定
// 「Shift+拖选」「Ctrl+滚轮」这类指针手势能否成立。
//
// 本条守两件事：
// ① **鼠标消息不得意外清空 tracker**。指针捕获变化（SetCapture / ReleaseCapture）在
//    `on_mouse` 内发生，Win32 不为它发键盘消息——若哪天有人把「指针捕获变化也清空」照搬
//    键盘的失焦清空策略进来，Shift+拖选跨出窗口就会丢掉 Shift，而这种回归在真机上只表现为
//    「拖选偶尔不扩展」，极难定位。
// ② **指针读到的是当前 tracker 态而非某个快照**：按下 Shift 之后产生的鼠标事件必须带上它。
AURORA_TEST_CASE(pointer_events_consume_get_without_disturbing_the_tracker) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    // 基线：重激活时的一次异步播种（覆盖「Alt+Tab 切进来时 Alt 仍按着」）。
    t.seed(ModifierKey::Shift | ModifierKey::Alt);
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt);

    // ① 鼠标消息进消息流：VK_LBUTTON 非修饰键，apply 翻不了任何位，get() 的值原样保留。
    //    指针事件正是读这个 get()——读到的仍是 Shift|Alt。
    AURORA_TEST_CHECK_FALSE(t.apply(VK_LBUTTON, true));
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt);
    AURORA_TEST_CHECK_FALSE(t.apply(VK_RBUTTON, false));
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt);
    AURORA_TEST_CHECK_FALSE(t.apply(VK_MBUTTON, true));
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt);
    //    指针捕获变化（WMD_SETCURSOR / WM_MOUSEACTIVATE 之类）在实现里不清空 tracker：
    //    键盘焦点未动、抬起消息仍进本窗口，清空反而会丢 Shift。此处显式钉住该策略。
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt);

    // ② 指针事件读到的是**当前**态：此后按下 Ctrl，紧接着的鼠标事件必须带 Control。
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, true));
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt | ModifierKey::Control);
    AURORA_TEST_CHECK_FALSE(t.apply(VK_LBUTTON, false));
    check_mods(t, ModifierKey::Shift | ModifierKey::Alt | ModifierKey::Control);

    // ③ 键盘路径与指针路径读的是同一个 get()，两侧逐位相等（同一份真值源，无第二来源）。
    ModifierKeyTracker for_key;
    ModifierKeyTracker for_pointer;
    for_key.seed(ModifierKey::Meta);
    for_pointer.seed(ModifierKey::Meta);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(for_key.get()), static_cast<std::uint8_t>(for_pointer.get()));
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

// Win32 导航区把主键盘与小键盘**共用同一组 VK**，判据只能落在 lParam 的扫描码上。
// 这组用例锁住该判据的**实测结论**：六个导航键里只有 `Home` 真的可分。
//
// 为什么这条值得单测：把「带 E0 前缀」当成「来自小键盘」是**错的**——主键盘的 PgUp / PgDn
// 本身就带 E0 前缀（scan 0x49 / 0x51，两边完全相同），那样判会把主键盘那两个键一起误判成
// `KP_Prior` / `KP_Next`。误判比不判更糟，故判据必须保守到「只认 Home 的扫描码差异」。
AURORA_TEST_CASE(numpad_nav_scan_separates_only_the_keys_it_really_can) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 唯一真可分的一键：小键盘 Home = E0 4E，主键盘 Home = 47。
    AURORA_TEST_CHECK_TRUE(detail::is_numpad_nav_scan(VK_HOME, 0x4E));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_HOME, 0x47));
    // 五个不可分的键：主键盘那侧（即便扫描码与小键盘相同）一律判 false。
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_END, 0x4F));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_PRIOR, 0x49));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_NEXT, 0x51));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_INSERT, 0x52));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_DELETE, 0x53));
    // 主键盘方向键与小键盘方向键：判据只认 Home，其它一律 false。
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_UP, 0x48));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_LEFT, 0x4B));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_DOWN, 0x50));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_RIGHT, 0x4D));
    // 非导航键：小键盘 0-9 与运算键各有独立 VK，不走这条判据。
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_NUMPAD5, 0x4C));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(VK_ADD, 0x4E));
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(0x41, 0x1E));  // 字母 A
    AURORA_TEST_CHECK_FALSE(detail::is_numpad_nav_scan(0x41, 0x4E));
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(scan_code_of_reads_only_the_scan_byte_field) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // scan_code_of 只取第 16-23 位，不受高位标志位（extended / 上下文码 / 前后态）影响。
    AURORA_TEST_CHECK_EQ(static_cast<int>(detail::scan_code_of(make_keydown_lparam(0x4E, true))), 0x4E);
    AURORA_TEST_CHECK_EQ(static_cast<int>(detail::scan_code_of(make_keydown_lparam(0x4E, false))), 0x4E);
    AURORA_TEST_CHECK_EQ(static_cast<int>(detail::scan_code_of(make_keydown_lparam(0x53, true))), 0x53);
    AURORA_TEST_CHECK_EQ(static_cast<int>(detail::scan_code_of(make_keydown_lparam(0x00, true))), 0x00);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

// Win32 VK → KeyCode 映射表。这组用例的价值在于**变异自证**：删掉任一 KP_* 的 case，
// 对应断言立刻变红——映射表错位在运行期只表现为「某个键没反应」，没有单测就只能靠人工撞。
AURORA_TEST_CASE(win32_vk_maps_the_keypad_digits_and_operators) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 小键盘 0-9：各 VK 连号（0x60..0x69），与主键盘数字行（'0'..'9'）**分得开**。
    for (int i = 0; i <= 9; ++i) {
        AURORA_TEST_CHECK(detail::from_win32_vk(VK_NUMPAD0 + i, false) ==
                          static_cast<KeyCode>(static_cast<int>(KeyCode::KP_0) + i));
    }
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_ADD, false) == KeyCode::KP_Add);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_SUBTRACT, false) == KeyCode::KP_Subtract);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_MULTIPLY, false) == KeyCode::KP_Multiply);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DIVIDE, false) == KeyCode::KP_Divide);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DECIMAL, false) == KeyCode::KP_Decimal);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_SEPARATOR, false) == KeyCode::KP_Separator);
    // 主键盘数字行不得被小键盘 VK 污染：'0' 仍是 D0、'9' 仍是 D9。
    AURORA_TEST_CHECK(detail::from_win32_vk('0', false) == KeyCode::D0);
    AURORA_TEST_CHECK(detail::from_win32_vk('9', false) == KeyCode::D9);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(win32_vk_splits_keypad_home_and_keeps_the_rest_on_the_mainboard_codes) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 唯一真可分的一键：小键盘 Home 给 KP_Home，主键盘 Home 仍给 Home。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_HOME, true) == KeyCode::KP_Home);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_HOME, false) == KeyCode::Home);
    // 其余五个导航键：即便 from_numpad 为真也**不给** KP_*（判据在真实链路上根本不会为真），
    // 恒给主键码 —— 守住「不误判主键盘」这条比「多给一个小键盘码位」重要的取舍。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_END, true) == KeyCode::End);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_PRIOR, true) == KeyCode::PageUp);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_NEXT, true) == KeyCode::PageDown);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DELETE, true) == KeyCode::Delete);
    // Insert 同族：即便 `from_numpad` 为真也**不给** `KP_Insert`，恒给主档 `Insert`
    // （NumLock 关闭时小键盘 0 与主键盘 Insert 打同一个 VK，不做扫描码二次判定；
    // 判据与取舍见下面 `win32_vk_maps_insert_to_the_mainboard_code`）。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_INSERT, true) == KeyCode::Insert);
    // 小键盘方向键（中央倒 T 簇）在 KeyCode 里无 KP_ 码位，与主键盘同码。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_UP, true) == KeyCode::ArrowUp);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_LEFT, true) == KeyCode::ArrowLeft);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DOWN, true) == KeyCode::ArrowDown);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_RIGHT, true) == KeyCode::ArrowRight);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

// `VK_INSERT` 恒给主档 `KeyCode::Insert`：主键盘 Insert 与小键盘 Insert（NumLock 关闭时的 `KP_0`）
// 在 Win32 上发**同一个 VK**，来处只在 lParam 的扫描码 / extended 位里，而那套判据在导航区其余
// 键上已被实测证伪（见 `is_numpad_nav_scan` 的对照表）。故此处**不做**二次判定——`from_numpad`
// 为 false 与 true 两条都必须落到 `Insert`，这是本条判据的本体（只钉一条挡不住「顺手加个
// `from_numpad ? KP_Insert : Insert`」的写法，而那正是本条明确否决的口径）。
AURORA_TEST_CASE(win32_vk_maps_insert_to_the_mainboard_code) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_INSERT, false) == KeyCode::Insert);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_INSERT, true) == KeyCode::Insert);
    // 主档与小键盘档不可互换：本后端恒不产 `KP_Insert`（与 `KP_End` / `KP_Prior` 等同族口径）。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_INSERT, true) != KeyCode::KP_Insert);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_INSERT, false) != KeyCode::KP_Insert);
    // 与相邻的 Delete 互不串码（两者在 Win32 上也是两个独立 VK，此处仅防手滑写反）。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DELETE, false) == KeyCode::Delete);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_DELETE, false) != KeyCode::Insert);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(win32_vk_keeps_the_existing_mainboard_mapping) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 本轮只增小键盘，既有主键盘映射必须逐位不变（`from_numpad` 为 false 一律走原分支）。
    AURORA_TEST_CHECK(detail::from_win32_vk('A', false) == KeyCode::A);
    AURORA_TEST_CHECK(detail::from_win32_vk('Z', false) == KeyCode::Z);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_RETURN, false) == KeyCode::Enter);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_ESCAPE, false) == KeyCode::Escape);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_TAB, false) == KeyCode::Tab);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_SPACE, false) == KeyCode::Space);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_F1, false) == KeyCode::F1);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_F12, false) == KeyCode::F12);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_OEM_PLUS, false) == KeyCode::Equal);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_OEM_5, false) == KeyCode::Backslash);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_LWIN, false) == KeyCode::Meta);
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_MENU, false) == KeyCode::Alt);
    // 未收录的键落 Unknown（`VK_NONAME` 之类的哨兵码不在任何 case 内）。
    AURORA_TEST_CHECK(detail::from_win32_vk(VK_NONAME, false) == KeyCode::Unknown);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

// ---------------------------------------------------------------------------
// 锁定态位（NumLock）的记账：toggle 语义 + 与按住态分离
//
// 背景：`async_modifiers()` 只在拿到前台时播种一次锁定位，此后该位在派发期是陈旧值——用户
// 切换 NumLock，事件上的位不变，且失焦的 `clear()` 会把它抹成「关」。现改为随消息流推进：
// `apply` 的第二段按 toggle 处理，`clear()` 只清按住态。
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(lock_bit_is_not_routed_through_bit_for) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 两条判据分离是结构保证：锁定位是 toggle（按下即翻转），若混进 bit_for 的置位/清位模型，
    // 一次 Down+Up 会净翻转零次，该位永远停在初值。
    AURORA_TEST_CHECK_FALSE(ModifierKeyTracker::bit_for(VK_NUMLOCK).has_value());
    const auto lock = ModifierKeyTracker::lock_bit_for(VK_NUMLOCK);
    AURORA_TEST_REQUIRE(lock.has_value());
    AURORA_TEST_CHECK(*lock == ModifierKey::NumLock);
    // 按住态那四个仍只归 bit_for，lock_bit_for 不认它们（否则一次 Shift+Tab 会去翻锁定位）。
    for (const int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU,
                         VK_RMENU, VK_LWIN, VK_RWIN}) {
        AURORA_TEST_CHECK(ModifierKeyTracker::bit_for(vk).has_value());
        AURORA_TEST_CHECK_FALSE(ModifierKeyTracker::lock_bit_for(vk).has_value());
    }
    // 非修饰、非锁定的键两侧都不命中（裸码 0x41 = 'A'，见下方用例的同款说明）。
    AURORA_TEST_CHECK_FALSE(ModifierKeyTracker::bit_for(0x41).has_value());
    AURORA_TEST_CHECK_FALSE(ModifierKeyTracker::lock_bit_for(0x41).has_value());
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(numlock_down_toggles_the_lock_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 翻转而非置位：NumLock 是切换键，GetKeyState 的 bit0 就是它自己的指示位，
    // 按下消息到达即代表用户刚切换过一次。第二次按下（用户再按一次关掉）必须回到关。
    ModifierKeyTracker t;
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_NUMLOCK, true));
    check_mods(t, ModifierKey::NumLock);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_NUMLOCK, true));
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_NUMLOCK, true));
    check_mods(t, ModifierKey::NumLock);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(numlock_down_up_pair_flips_exactly_once) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 防 Up 二次翻转：一次 Down + 一次 Up = 一次物理动作，净翻转必须是**一次**而不是两次或零次。
    // 自动重复（按住不放连发 WM_KEYDOWN）在真实 Win32 上同样只算用户的一次切换意图。
    ModifierKeyTracker t;
    t.apply(VK_NUMLOCK, true);
    t.apply(VK_NUMLOCK, false);
    check_mods(t, ModifierKey::NumLock);
    // 第二对：再翻回关。
    t.apply(VK_NUMLOCK, true);
    t.apply(VK_NUMLOCK, false);
    check_mods(t, ModifierKey::None);
    // Up 先行（无 Down）时不得翻转：净翻转零次。
    ModifierKeyTracker lone;
    AURORA_TEST_CHECK_TRUE(lone.apply(VK_NUMLOCK, false));
    check_mods(lone, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(numlock_toggle_leaves_the_held_bits_untouched) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 判据 B 的核心：用户切 NumLock 时通常同时按着 Ctrl/Shift（NumLock 就在小键盘边上），
    // 按住态位必须逐位不变。锁定位与按住态是两份独立记账。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    t.apply(VK_SHIFT, true);
    check_mods(t, ModifierKey::Control | ModifierKey::Shift);
    // 一对 Down/Up = 一次物理切换 = 净翻转一次，故该位**停在开**，不是回到关。
    t.apply(VK_NUMLOCK, true);
    check_mods(t, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock);
    t.apply(VK_NUMLOCK, false);
    check_mods(t, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock);
    // 再一对翻回关，按住态自始至终逐位不变。
    t.apply(VK_NUMLOCK, true);
    t.apply(VK_NUMLOCK, false);
    check_mods(t, ModifierKey::Control | ModifierKey::Shift);
    // 按住态自己的抬起仍只清自己那位，锁定位不受牵连。
    t.apply(VK_CONTROL, false);
    check_mods(t, ModifierKey::Shift);
    t.apply(VK_SHIFT, false);
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(clear_drops_held_bits_but_keeps_the_lock_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // clear() 服务的是幻影防护，而幻影的成因是「抬起消息投给了别的窗口」——锁定态没有抬起消息
    // 可言，NumLock 也不因本窗口失焦而改变。抹成「关」是假报（这不是「取不到」，是被无条件清零）。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    t.apply(VK_NUMLOCK, true);
    check_mods(t, ModifierKey::Control | ModifierKey::NumLock);
    t.clear();
    check_mods(t, ModifierKey::NumLock);  // 按住态已清、锁定位保留
    // 交出前台前未送达的抬起消息回来时也不该把锁定位「抬」掉（Up 本就不翻）。
    AURORA_TEST_CHECK_TRUE(t.apply(VK_NUMLOCK, false));
    check_mods(t, ModifierKey::NumLock);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(seed_splits_the_lock_bit_out_of_the_observed_mask) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // `async_modifiers()` 的读数本身就混装两类位（GetAsyncKeyState 的四个按住位 +
    // GetKeyState(VK_NUMLOCK) 的锁定指示位），故 seed 必须按掩码拆分写入。
    // 判据：拆分后 get() 逐位不变（对外形状不变），而 clear() 之后锁定位仍在——这正是
    // 「不整体覆盖」的证据：整体覆盖的写法在这里也能过 get()，但锁定位会与按住态同生共死。
    ModifierKeyTracker t;
    t.seed(ModifierKey::Shift | ModifierKey::Control | ModifierKey::NumLock);
    check_mods(t, ModifierKey::Shift | ModifierKey::Control | ModifierKey::NumLock);
    t.clear();
    check_mods(t, ModifierKey::NumLock);
    // 播种后切换 NumLock 仍能翻到「关」——锁定位不是被钉死在播种值上的。
    t.apply(VK_NUMLOCK, true);
    check_mods(t, ModifierKey::None);
    // 只有锁定位的读数：按下态为空、锁定位落位。
    ModifierKeyTracker only_lock;
    only_lock.seed(ModifierKey::NumLock);
    check_mods(only_lock, ModifierKey::NumLock);
    // 只有按住位的读数：锁定位为空。
    ModifierKeyTracker only_held;
    only_held.seed(ModifierKey::Alt);
    check_mods(only_held, ModifierKey::Alt);
    only_held.clear();
    check_mods(only_held, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(non_modifier_keys_leave_both_states_alone) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 非修饰键走 apply 仍返回 false，且**两份**状态都不改（含已置位的锁定位）。
    // 用裸码 0x41（'A'）而非 `VK_A`：MinGW 的 <windows.h> 不定义字母类 VK_ 宏（既有
    // `non_modifier_keys_never_touch_the_state` 同款写法），而 tracker's 判据只比数值。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    t.apply(VK_NUMLOCK, true);
    const auto before = static_cast<std::uint8_t>(t.get());
    AURORA_TEST_CHECK_FALSE(t.apply(0x41, true));
    AURORA_TEST_CHECK_FALSE(t.apply(0x41, false));
    AURORA_TEST_CHECK_FALSE(t.apply(VK_SPACE, true));
    // 大写锁定既不在按住态表也不在锁定态表（`VK_CAPITAL` 未建模），不得污染任一份状态。
    AURORA_TEST_CHECK_FALSE(t.apply(VK_CAPITAL, true));
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(t.get()), before);
    check_mods(t, ModifierKey::Control | ModifierKey::NumLock);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

#undef AURORA_WIN32_MODIFIERS_SKIP

}  // namespace aurora::test_cases::utest_win32_modifiers
