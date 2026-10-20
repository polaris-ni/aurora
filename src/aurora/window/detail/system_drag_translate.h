#pragma once

// 系统拖放折算层：**内部头**（与 `ime_composition.h` / `win32_ime.h` 同列于 src/）。
//
// 为何单列且零平台依赖：四个平台的拖放 API 对同一件事给的数据形态**都不相同**——Win32
// 给 `IDataObject::EnumFormatEtc` 的 `CLIPFORMAT` 数值与 `IDropTarget::DragOver` 的 `DROPEFFECT`
// 位、X11 给 `XdndEnter` 的 atom 清单与 `XdndAction*` 位、Wayland 给 `wl_data_offer.offer` 的
// MIME 串与 `dnd_action`、macOS 给 `NSPasteboard` 类型与 `NSDragOperation`。而 Aurora 的契约是
// **`SystemDropEvent` + MIME 清单 + `DragAction`**（见 `event/event.h`）。故「平台原始值 → 契约值」
// 这层折算必须存在，且必须是**可无头单测**的纯函数：OS 会话不可自动化，格式与动作的错位只有
// 纯函数测试才兜得住（CF_DIBV5 ↔ PNG、DROPEFFECT 位序、MK_* 位与锁定态污染是三类最易错输入）。
//
// 本头不引任何 `<windows.h>` / Xlib / wayland / Cocoa 头：Win32 侧入参一律用**数值**
// （`CLIPFORMAT` 是 `WORD`、`MK_*` 与 `DROPEFFECT` 是 `DWORD`），由平台桥原样转传；常量在本地按
// 数值语义声明（理由同 `ime::Attr`：各 SDK 命名不一，折算层只依赖数值）。
//
// 门控：无。本头在**所有**平台编译（含 wasm），以便单测在任何 CI 作业上都能跑。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/event/event.h"

namespace aurora::drag {

/// @brief 同步 / 异步读取的分界阈值（字节）：**四个平台桥共用的单一真源**。
///
/// 低于或等于它的载荷走同步通道（`DragPayload::from_bytes`）——跨进程 IO 在这一量级上撑不满一帧；
/// 超过它走 `read_async` + `system_drag::pump_reads()`，读取期间帧循环继续推进。散在四个桥里各写
/// 一个数是双真源：同一份载荷在不同平台上走不同通道，行为分叉且无从断言。
inline constexpr std::size_t AURORA_DRAG_SYNC_PAYLOAD_LIMIT = static_cast<std::size_t>(64U) * 1024U;

/// @brief Win32 `IDropTarget` 回调的按键状态位（`grfKeyState`；MSDN「MK_ Constants」原文数值）。
///
/// 本地声明而非取 SDK 宏：折算层不引 `<windows.h>`（详见文件头）。该位集**不含 Meta**——
/// Win 键在拖放会话期间无从由 `MK_*` 表达，故 `ModifierKey::Meta` 在 Win32 拖放路径上恒不置位
/// （如实不为，不猜）。
enum class Win32MkFlag : std::uint8_t {
    Shift = 0x0004U,  ///< `MK_SHIFT`
    Control = 0x0008U,  ///< `MK_CONTROL`
    Alt = 0x0020U,  ///< `MK_ALT`
};

/// @brief Win32 剪贴板格式数值（`CLIPFORMAT`；MSDN「Standard Clipboard Formats」原文数值）。
///
/// 只登记本库会消费的三类：文件清单、Unicode 文本、位图。注册名格式（`CF_RTF` 一类字符串
/// 注册格式）按名映射，不在本表内。
enum class Win32ClipFormat : std::uint8_t {
    UnicodeText = 13U,  ///< `CF_UNICODETEXT`
    Hdrop = 15U,  ///< `CF_HDROP`（文件清单）
    DibV5 = 17U,  ///< `CF_DIBV5`（含 alpha 的位图）
};

/// @brief Win32 `DROPEFFECT` 位（`IDropTarget` 的 `pdwEffect` 与 `DoDragDrop` 返回值；原文数值）。
enum class Win32DropEffect : std::uint8_t {
    None = 0U,  ///< `DROPEFFECT_NONE`：不接受
    Copy = 1U,  ///< `DROPEFFECT_COPY`
    Move = 2U,  ///< `DROPEFFECT_MOVE`
    Link = 4U,  ///< `DROPEFFECT_LINK`
};

/// @brief Win32 `grfKeyState`（`MK_*` 位集）→ `ModifierKey` 位集。
///
/// 只置可按住的位：`MK_*` 本身不含锁定态（NumLock 一类不在其中），故无需摘掩码；折算出的位集
/// 天然落在 `AURORA_MODIFIER_PRESSABLE_MASK` 之内。
/// @param flags 平台侧 `grfKeyState` 原值（未知位忽略）。
/// @return 按下的修饰位并集；无修饰键时为 `ModifierKey::None`。
[[nodiscard]] auto modifiers_from_win32_mk_flags(std::uint32_t flags) -> ModifierKey;

/// @brief Win32 `CLIPFORMAT` → MIME；未登记格式返回 `std::nullopt`（不猜、不兜底成空串）。
/// @param format 平台侧剪贴板格式数值。
/// @return 对应 MIME（取自 `app/mime_types.h` 常量）；未登记时为 `std::nullopt`。
[[nodiscard]] auto mime_from_win32_clip_format(std::uint16_t format) -> std::optional<std::string>;

/// @brief MIME → Win32 `CLIPFORMAT`（上一函数的逆）；未登记 MIME 返回 `std::nullopt`。
/// @param mime 待映射的 MIME（大小写敏感，须为 `app/mime_types.h` 登记的规范形）。
/// @return 对应剪贴板格式数值；未登记时为 `std::nullopt`。
[[nodiscard]] auto win32_clip_format_from_mime(std::string_view mime) -> std::optional<std::uint16_t>;

/// @brief Win32 剪贴板格式清单 → MIME 清单（保持输入顺序、去重、丢弃未登记项）。
///
/// 顺序即优先级（源侧声明顺序），折算不得重排——`query_system_drag_accept` 与载荷读取都按该顺序
/// 取首选格式。
/// @param formats 平台侧格式数值清单（可含重复与未登记项）。
/// @return 去重后的 MIME 清单（未登记项被丢弃，可为空）。
[[nodiscard]] auto mimes_from_win32_clip_formats(const std::vector<std::uint16_t> &formats) -> std::vector<std::string>;

/// @brief `DragAction` → Win32 `DROPEFFECT` 位（光标反馈回写用）。
/// @param action 目标侧允许 / 建议的动作。
/// @return `DROPEFFECT` 位值；`None` 时为 `DROPEFFECT_NONE`（光标显示「禁止放置」）。
[[nodiscard]] auto win32_drop_effect_from_action(DragAction action) -> std::uint32_t;

/// @brief Win32 `DROPEFFECT` 位 → `DragAction`（上一函数的逆，多位时按固定优先序取一）。
///
/// OS 可能同时给出多个位（源侧声明「可复制亦可移动」）。优先序 **Copy → Move → Link**：
/// 与 `proposed_action_for` 的「无修饰键按缺省 Copy」口径同源，且复制是最不易造成数据损失的
/// 解读。取不到任何已知位时为 `None`。
/// @param effect 平台侧 `DROPEFFECT` 原值（未知位忽略）。
/// @return 译出的动作；无已知位时为 `DragAction::None`。
[[nodiscard]] auto action_from_win32_drop_effect(std::uint32_t effect) -> DragAction;

/// @brief 客户区**物理像素** → 窗口**逻辑 dp**（拖放坐标契约的唯一折算入口）。
///
/// 与 `Win32Host::Impl::to_logical` 同款但可无头验证：`scale` 由调用方（宿主）给出，本函数不读
/// 任何平台 DPI——读 DPI 是宿主内 `refresh_scale()` 的独占职责，折算层再读一次就是双真源。
/// @param x 客户区横坐标（物理像素）。
/// @param y 客户区纵坐标（物理像素）。
/// @param scale 当前 DPI 缩放（dp → 物理像素）；≤ 0 时按 1.0 处理（不产生 NaN / Inf）。
/// @return 逻辑坐标点。
[[nodiscard]] auto logical_from_client_px(int x, int y, float scale) -> Point;

/// @brief 平台侧一次拖入回调的原始数据（**平台无关形态**，由平台桥填好后交折算层）。
///
/// 字段全部是可由纯值表达的形态：平台桥只做「取原生值 → 填本结构」，语义翻译全在折算层，
/// 于是翻译逻辑可无头单测、平台桥退化为无语义的搬运。
struct DragInFrame {
    /// @brief 平台侧可用格式的数值清单（Win32 为 `CLIPFORMAT`；其余平台按同一位置填其原生值）。
    std::vector<std::uint16_t> clip_formats;
    ModifierKey modifiers = ModifierKey::None;  ///< 修饰键位集（由平台侧状态位折算而来）
    /// @brief OS 报告的源侧建议动作（拖入侧的 `default_action` 来源；`None` = OS 未报告）。
    ///
    /// 拖出侧该值取自 `DragOffer::default_action`，拖入侧源是外部进程、没有 `DragOffer`，故只能取
    /// OS 报告值——这是「拖入侧缺省动作」的唯一真值源，不得另设第二处。
    DragAction os_suggested = DragAction::None;
    int client_x = 0;  ///< 指针横坐标（客户区物理像素）
    int client_y = 0;  ///< 指针纵坐标（客户区物理像素）
    float scale = 1.0F;  ///< 当前 DPI 缩放（dp → 物理像素）
};

/// @brief 由平台侧原始数据构造 `SystemDropEvent`（不填载荷）。
///
/// 折算式完成三件事：坐标（客户区像素 → 逻辑 dp）、formats（格式数值 → MIME 清单）、
/// `proposed`（修饰键 + OS 报告值 → 建议动作，走 `proposed_action_for` 唯一翻译点）。
/// `allowed` 与 `payload` 不在本函数职责内：`allowed` 由**派发器**按 accept 查询与 `proposed`
/// 求交后写定，载荷由调用方按阶段装配（`Enter` / `Over` / `Leave` 可无载荷，`Drop` 才有）。
/// @param phase 本次回调的阶段。
/// @param frame 平台侧原始数据。
/// @return 填好 `phase` / `position` / `formats` / `modifiers` / `proposed` 的事件。
[[nodiscard]] auto make_drop_event(SystemDropPhase phase, const DragInFrame &frame) -> SystemDropEvent;

}  // namespace aurora::drag
