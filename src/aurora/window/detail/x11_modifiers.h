#pragma once

// X11 事件修饰态掩码 → aurora ModifierKey 的折算：**内部头**（与 `win32_modifiers.h` /
// `win32_keymap.h` 同列于 src/aurora/window/detail/，不进 include/），故可被单测直接吃表。
//
// 为什么要独立成头而不是留在 `x11_surface.cpp` 里：这张折算表是「X 修饰态 ↔ aurora 语义位」
// 的唯一实现，Mod1 到底是 Alt 还是别的、Mod4 是不是 Meta，一旦错位只表现为「某个组合不生效」，
// 极难定位。折算函数本身是**纯函数**（只读一个入参、不读任何状态），放进可单测的内部头后，
// 「Mod1=Alt / Mod4=Meta」这类既有口径就能被 CTest 逐位钉住。
//
// 为何自带掩码常量而不引 Xlib 头（照 `keysym_map.h` 的做法）：`<X11/X.h>` 会把 `None` /
// `Status` / `Bool` / `True` / `False` 等通用词定义为宏（`x11_surface.cpp` 为此专门做了
// 取值后再 `#undef None` 的处理），把它拉进头文件会连带污染所有包含者；而这里的取值需求
// 只有五个位，X11 协议把它们定为**永不改变的 ABI 常量**，自带一份即可。
//
// 真实性的保证不靠「文档说是这样」：`x11_surface.cpp` 编译时对每个常量与真实 Xlib 宏做了
// `static_assert` 对照（见该文件），一旦上游改值即编译失败，不会静默错位。
//
// 门控与 `win32_modifiers.h` 同款：平台宏 ∧ 后端宏（X11 走经典 Xlib 事件，不含 XI2）。
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

#include <cstdint>

#include "aurora/event/event.h"

namespace aurora::detail {

// X11 核心协议的修饰态掩码（X11 ABI 稳定常量，数值取自 `<X11/X.h>` 的同名宏）。
// 逐项语义见下方 `//` 注释——这些是命名空间内的裸常量而非可文档化声明，按本仓
// `check_doc_comments` 的口径（DOC-R3）挂 `///` 会被判为孤立文档块，故用普通注释。
//
// 这些位编进**每一个**核心事件的 `state` 字段（`XKeyEvent` / `XButtonEvent` / `XMotionEvent`
// 共用同一布局），所以键盘与指针路径读的是同一个字段、同一种口径。
namespace x11_state_mask {

// Shift（任一侧）。X 不分左右，本就只有一个 Shift 位。
inline constexpr unsigned int kShift = 1U << 0U;
// Caps Lock。**未建模**：锁定态对指针与键盘事件均无消费方，不静默折成某个语义位。
inline constexpr unsigned int kLock = 1U << 1U;
// Control（任一侧）。
inline constexpr unsigned int kControl = 1U << 2U;
// `Mod1`，X 惯例映射为 **Alt**（PC 键盘约定；非 PC 布局可能是别的键，本库按 PC 惯例取用）。
inline constexpr unsigned int kMod1 = 1U << 3U;
// `Mod2`，PC 键盘约定上是 **NumLock**。它不是「按住态」而是「锁定态」，但 X 把锁定态也编进
// `state`，故与其它位同口径取用、无需额外查询。
inline constexpr unsigned int kMod2 = 1U << 4U;
// `Mod3`，PC 键盘约定上是 AltGr 的第三段（无消费方，不建模）。
inline constexpr unsigned int kMod3 = 1U << 5U;
// `Mod4`，X 惯例映射为 **Super / Meta**（对应 Windows 的 Win 键、macOS 的 Command）。
inline constexpr unsigned int kMod4 = 1U << 6U;

}  // namespace x11_state_mask

/// @brief 事件的 `state` 掩码 → 修饰键位组合。
///
/// 口径要点：
/// - **Mod1 = Alt、Mod4 = Meta** 是 X 的 PC 惯例，本库沿用；消费方按 `& ModifierKey::Alt`
///   判定即可，不需要知道平台差异。
/// - `Mod2` 折成 `ModifierKey::NumLock`：它是键盘锁定位，与指针事件无语义关系，但本函数
///   是键盘与指针**共用**的唯一折算入口（两处 `state` 字段语义相同），故不在指针路径上
///   另开一份裁剪逻辑——指针事件照实携带该位，消费方只看四个可按住位时自行掩码。
/// - `Mod1`/`Mod3` 之外的位（`Lock`、`Mod5`）无消费方，一律不折，绝不静默假报。
/// - 输入 0（消息不带状态，如 `LeaveNotify`）返回 `None`，即「不可知即无修饰」。
///
/// @param state 事件的 `state` 字段（`XKeyEvent` / `XButtonEvent` / `XMotionEvent` 共用）。
/// @return 对应的位集（组合值，非单个枚举量）。
[[nodiscard]] constexpr auto mods_from_x11_state(unsigned int state) -> ModifierKey {
    auto m = ModifierKey::None;
    if ((state & x11_state_mask::kShift) != 0U) {
        m = m | ModifierKey::Shift;
    }
    if ((state & x11_state_mask::kControl) != 0U) {
        m = m | ModifierKey::Control;
    }
    if ((state & x11_state_mask::kMod1) != 0U) {
        m = m | ModifierKey::Alt;
    }
    if ((state & x11_state_mask::kMod4) != 0U) {
        m = m | ModifierKey::Meta;
    }
    if ((state & x11_state_mask::kMod2) != 0U) {
        m = m | ModifierKey::NumLock;
    }
    return m;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_LINUX && !AURORA_PLATFORM_ANDROID && AURORA_BACKEND_X11
