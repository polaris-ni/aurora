#pragma once

// Win32 修饰键跟踪器：**内部头**（与 `win32_ime.h` 同列于 src/，不进 include/）。
//
// 为何不在派发时刻用 `GetAsyncKeyState` 现场采样：异步采样读的是「调用那一刻」的物理按键态，
// 而消息从队列取走到此处可能已晚于一帧 —— UI 卡顿、或人手 chord 短于一帧（约 21–30 ms）时
// 修饰键早已抬起，热键于是**静默**丢失（实测：冻住 UI 线程后注入 45 ms 的人手节奏 chord，
// 解冻后 0/10 命中）。本跟踪器把修饰态改为**随消息流推进**：按下置位、抬起清位，于是每个
// `KeyEvent` 带的是它与该键在队列里严格对应的那一刻的状态，与泵送延迟无关。
//
// 幻影防护：失去激活/焦点时整体清空（抬起消息可能已投给别的窗口），重新获得激活时用一次
// 异步采样播种基线（覆盖「Alt+Tab 切进来时 Alt 仍按着」）。指针捕获变化**不**清空——键盘焦点
// 未动，抬起消息仍会进本窗口，清空反而会把 Shift+拖选期间仍按住的 Shift 误丢掉。
//
// 两类位分开记账：`state_` 是**按住态**（按下置位、抬起清位、清空即上面的幻影防护）；`locks_`
// 是**锁定态**（`NumLock` 这类切换键按下即翻转，没有抬起消息可言，故**不参与**清空——把它抹成
// 「关」是假报）。`get()` 回两者的并集，故事件字段的对外形状逐位不变。`bit_for` 与
// `lock_bit_for` 是两条分开的判据：混进一条会让 toggle 键被按「置位/清位」模型处理，一次
// Down+Up 净翻转零次（锁定位永远停在初值）。
//
// 不区分左右：`VK_LSHIFT` / `VK_RSHIFT` / `VK_SHIFT` 折到同一位。注入与某些布局会发出不配对的
// 变体（按下给 L 变体、抬起给通用码），分侧记账必然残留幻影。AltGr 同理（它本身就是
// `VK_RMENU` + `VK_LCONTROL` 两段序列，折位后与右 Alt 一致，符合 Windows 上的常规语义）。
// 折位的代价：两侧齐按后先抬其一会提前丢位（另一侧其实仍物理按着）。这是刻意取舍——分侧记账
// 换来的幻影由「不配对变体」这条**常态**路径触发，而丢位需要「两侧同按再抬一侧」这种指法。
//
// 门控与 `win32_ime.h` 同款：平台宏 ∧ 后端宏析取（Win32 GDI 与 D3D11 共用宿主）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming)
#endif

#include <windows.h>

#include <cstdint>
#include <optional>

#include "aurora/event/event.h"

namespace aurora::detail {

/// @brief 宿主端修饰键状态机：Win32 虚拟键码的按下/抬起 → `ModifierKey` 位集。
/// @note Thread: main-thread only（消息泵线程）
/// @note Side-effects: stateful（`apply` / `seed` / `clear` 改内部状态，`get` 纯读）
class ModifierKeyTracker {
  public:
    /// @brief 按一条键盘消息推进对应修饰位。
    ///
    /// 三段分派，顺序不可调换：
    ///  1. 命中 `bit_for`（按住态）→ 现行置位 / 清位；
    ///  2. 否则命中 `lock_bit_for`（锁定态）→ **仅 `down == true` 时翻转该位**，`down == false`
    ///     什么都不做；
    ///  3. 两者都不命中 → 返回 `false`、状态不变。
    ///
    /// @param vk 消息 `wParam` 的虚拟键码
    /// @param down true = 按下（`WM_KEYDOWN` / `WM_SYSKEYDOWN`），false = 抬起
    /// @return 该键是否归修饰态记账（false 时状态不变；调用方不据此决定是否放行消息——
    ///         `WM_KEY*` 恒派发；`WM_SYSKEY*` 的派发与否由 `syskey_dispatches()` 判定）。
    ///         切换键的按下消息本就仍要被记账，故其返回值同样是 `true`
    auto apply(int vk, bool down) -> bool {
        const auto bit = bit_for(vk);
        if (bit.has_value()) {
            if (down) {
                state_ = state_ | *bit;
            } else {
                // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位清余
                state_ = static_cast<ModifierKey>(static_cast<std::uint8_t>(state_) &
                                                  static_cast<std::uint8_t>(~static_cast<std::uint8_t>(*bit)));
            }
            return true;
        }
        const auto lock = lock_bit_for(vk);
        if (lock.has_value()) {
            // NumLock 是 toggle：GetKeyState 的 bit0 就是它自己的指示位，按下消息到达即代表用户
            // 刚切换过一次，故翻转而非置位。Up 不翻——一次 Down + 一次 Up 是一次物理动作，
            // Up 再翻就翻回原值。
            if (down) {
                // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位异或翻转
                locks_ = static_cast<ModifierKey>(static_cast<std::uint8_t>(locks_) ^ static_cast<std::uint8_t>(*lock));
            }
            return true;
        }
        return false;
    }

    /// @brief 用外部读数整体播种（获得激活时对物理态取一次基线）。
    ///
    /// 按掩码拆分写入：`async_modifiers()` 的读数本身就混装两类位（`GetAsyncKeyState` 的四个
    /// 按住位 + `GetKeyState(VK_NUMLOCK)` 的锁定指示位），故两类位各归其位、不整体覆盖。
    auto seed(ModifierKey observed) -> void {
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) 掩码枚举按位取子集，结果为合法组合值
        state_ = static_cast<ModifierKey>(static_cast<std::uint8_t>(observed) & AURORA_MODIFIER_PRESSABLE_MASK);
        locks_ = static_cast<ModifierKey>(static_cast<std::uint8_t>(observed) & AURORA_MODIFIER_LOCK_MASK);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    }

    /// @brief 清空**按住态**（失去激活 / 焦点：未送达的抬起消息不可追）。
    ///
    /// **锁定态刻意不清**：本方法服务的是幻影防护，而幻影的成因是「抬起消息投给了别的窗口」——
    /// 锁定态没有抬起消息可言，`NumLock` 也不会因本窗口失焦而改变。把锁定位一起抹成「关」是
    /// **假报**（这不是「取不到」，而是被无条件清零），且与「取不到时按关处理、不静默假报开」的
    /// 口径同源。锁定位另有 `seed()` 与 `apply()` 两条真值入口。
    auto clear() -> void { state_ = ModifierKey::None; }

    [[nodiscard]] auto get() const -> ModifierKey { return state_ | locks_; }

    /// @brief VK 码 → **按住态**修饰位（左右与通用码归并）。非修饰键回 nullopt。
    ///
    /// **刻意不含 `VK_NUMLOCK`**：锁定位是 toggle 语义（按下即翻转），与本函数的置位 / 清位
    /// 模型不同构，混进来会让一次 Down+Up 净翻转零次。它由 `lock_bit_for` 单独承载——两条判据
    /// 分离是「按住态」与「锁定态」不互相污染的结构保证。
    [[nodiscard]] static auto bit_for(int vk) -> std::optional<ModifierKey> {
        switch (vk) {
            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:
                return ModifierKey::Shift;
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL:
                return ModifierKey::Control;
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:
                return ModifierKey::Alt;
            case VK_LWIN:
            case VK_RWIN:
                return ModifierKey::Meta;
            default:
                return std::nullopt;
        }
    }

    /// @brief VK 码 → **锁定态**修饰位。非锁定类切换键回 nullopt。
    ///
    /// 当前只对 `VK_NUMLOCK` 返回 `ModifierKey::NumLock`：它是键盘上唯一的 NumLock 键，不存在
    /// 左右之分，故无需像 `bit_for` 那样归并多个 VK。将来建模 `CapsLock` / `ScrollLock` 时，
    /// 归 `AURORA_MODIFIER_LOCK_MASK` 的新位并在此表加分支。
    [[nodiscard]] static auto lock_bit_for(int vk) -> std::optional<ModifierKey> {
        switch (vk) {
            case VK_NUMLOCK:
                return ModifierKey::NumLock;
            default:
                return std::nullopt;
        }
    }

  private:
    ModifierKey state_ = ModifierKey::None;  ///< 按住态：随每条键盘消息置位 / 清位，失激活时整体清空
    ModifierKey locks_ = ModifierKey::None;  ///< 锁定态：切换键按下即翻转，不随失激活清空
};

/// @brief `WM_SYSKEY*` 的 VK 是否进入派发链（其余一律只推进修饰态后转交系统）。
///
/// `WM_SYSKEY*` 是「按住 Alt 期间的按键」，此前一律只借它推进修饰态、按键本身交
/// `DefWindowProcA`，代价是 Alt 组合在 Aurora 侧完全观察不到。现改为与常规键同路进派发：
/// 消费即止（`return 0`），未消费才回落 `DefWindowProcA`——系统菜单、菜单助记键、`Alt+F4`
/// 关闭等原生行为只有在「Aurora 侧不认领」时才发生，不会被无条件掐死。
///
/// **例外（只推进修饰态、不派发）**：
///   * `VK_MENU` / `VK_LMENU` / `VK_RMENU`（Alt 自身，左右都算）——它就是修饰键，
///     发出的那条 `KeyEvent` 语义为空，且会污染紧随其后的快捷键匹配；
///   * `VK_F10`（系统菜单键）——激活系统菜单属系统语义，无对应的 GUI 按键含义。
///
/// 判据是**纯函数**（不读任何状态），故可单测直接吃表：`WM_SYSKEY*` 的分叉只有这一处。
[[nodiscard]] inline auto syskey_dispatches(int vk) -> bool {
    switch (vk) {
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_F10:
            return false;
        default:
            return true;
    }
}

/// @brief 判「这条导航键消息是否来自数字小键盘导航区」——**只对 `Home` 成立**。
///
/// **背景**：Win32 在导航区把主键盘与小键盘**共用同一组虚拟键码**（`VK_HOME` / `VK_END` /
/// `VK_PRIOR` / `VK_NEXT` / `VK_INSERT` / `VK_DELETE` 两边都发这两个值），`wParam` 无从区分来处。
/// 唯一的额外信息是 `lParam` 里的扫描码与 extended（E0 前缀）位。
///
/// **实测结论（本条判据的全部依据）**：逐个核对标准 PC 键盘的 (VK, scan, extended) 组合后，
/// 六个导航键里**只有 `Home` 真的可分**——
///
/// | 键 | 主键盘 (scan, E0) | 小键盘 (scan, E0) | 可分？ |
/// |:---|:---|:---|:---|
/// | Home   | `0x47`, 0 | `0x4E`, 1 | **是**（扫描码不同） |
/// | End    | `0x4F`, 0 | `0x4F`, 1 | 否（仅 extended 位不同） |
/// | PgUp   | `0x49`, 1 | `0x49`, 1 | 否（完全相同） |
/// | PgDn   | `0x51`, 1 | `0x51`, 1 | 否（完全相同） |
/// | Insert | `0x52`, 0 | `0x52`, 1 | 否（仅 extended 位不同） |
/// | Delete | `0x53`, 0 | `0x53`, 1 | 否（仅 extended 位不同） |
///
/// 也就是说 **extended 位不足以判别**：主键盘的 PgUp / PgDn 本身就带 E0 前缀，把「带 E0」
/// 当成「来自小键盘」会把主键盘那两个键一起误判成 `KP_Prior` / `KP_Next`——那比不判更糟。
/// 故本判据**只认 `Home` 的扫描码差异**（`0x47` = 主键盘、`0x4E` = 小键盘），其余五个键一律
/// 返回 false，即恒给主键码、维持现网行为。
///
/// 代价是明确的：Win32 后端只产 `KP_Home`，`KP_End` / `KP_Prior` / `KP_Next` / `KP_Insert` /
/// `KP_Delete` 在本后端**恒不产生**。X11 / Wayland 侧 keysym 天然分得开、GLFW 侧键码表本身
/// 就缺这几项，故三后端键码集不完全一致——这是平台事实而非疏漏，`keycode.h` 的小键盘口径
/// 注释已写明。消费方需要这几项时应走 X11 / Wayland 后端，或按平台兜底。
///
/// 判据是纯函数（只读两个入参），故可单测直接吃表。
[[nodiscard]] constexpr auto is_numpad_nav_scan(int vk, unsigned char scan) -> bool {
    // 唯一真可分的一键：小键盘 Home 的 E0 4E 对主键盘 Home 的 47。
    return vk == VK_HOME && scan == 0x4E;
}

/// @brief 把 `WM_KEYDOWN` / `WM_SYSKEY*` 的 `lParam` 拆出扫描码字节（第 16-23 位）。
/// @param lp 消息 `lParam`。
/// @return 扫描码字节（0-255）。
[[nodiscard]] constexpr auto scan_code_of(LPARAM lp) -> unsigned char {
    return static_cast<unsigned char>((static_cast<unsigned long long>(lp) >> 16U) & 0xFFULL);
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_D3D11)
