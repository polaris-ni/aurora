// 系统拖放折算层实现：**零平台头**（契约与理由见同名头文件）。
//
// 本 TU 不引任何平台头：全部入参都是纯值（数值 / 枚举 / 字符串），故它在每个 CI 作业上都能编译，
// 单测也就能在任何平台上覆盖全部折算逻辑。

#include "system_drag_translate.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

#include "aurora/app/mime_types.h"

namespace aurora::drag {

namespace {
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位取子集，结果为合法组合值

/// @brief 按条件置位（避免每处都写一遍静态转换）。
/// @param base 起始位集。
/// @param bit 待置入的位。
/// @param on 是否置入。
/// @return 结果位集。
[[nodiscard]] auto with_bit(ModifierKey base, ModifierKey bit, bool on) -> ModifierKey {
    if (!on) {
        return base;
    }
    return static_cast<ModifierKey>(static_cast<std::uint8_t>(base) | static_cast<std::uint8_t>(bit));
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}  // namespace

auto modifiers_from_win32_mk_flags(const std::uint32_t flags) -> ModifierKey {
    ModifierKey mods = ModifierKey::None;
    mods = with_bit(mods, ModifierKey::Shift, (flags & static_cast<std::uint32_t>(Win32MkFlag::Shift)) != 0U);
    mods = with_bit(mods, ModifierKey::Control, (flags & static_cast<std::uint32_t>(Win32MkFlag::Control)) != 0U);
    mods = with_bit(mods, ModifierKey::Alt, (flags & static_cast<std::uint32_t>(Win32MkFlag::Alt)) != 0U);
    return mods;
}

auto mime_from_win32_clip_format(const std::uint16_t format) -> std::optional<std::string> {
    switch (format) {
        case static_cast<std::uint16_t>(Win32ClipFormat::UnicodeText):
            return std::string{AURORA_MIME_TEXT_PLAIN};
        case static_cast<std::uint16_t>(Win32ClipFormat::Hdrop):
            return std::string{AURORA_MIME_URI_LIST};
        case static_cast<std::uint16_t>(Win32ClipFormat::DibV5):
            return std::string{AURORA_MIME_IMAGE_PNG};
        default:
            return std::nullopt;
    }
}

auto win32_clip_format_from_mime(const std::string_view mime) -> std::optional<std::uint16_t> {
    if (mime == AURORA_MIME_TEXT_PLAIN) {
        return static_cast<std::uint16_t>(Win32ClipFormat::UnicodeText);
    }
    if (mime == AURORA_MIME_URI_LIST) {
        return static_cast<std::uint16_t>(Win32ClipFormat::Hdrop);
    }
    if (mime == AURORA_MIME_IMAGE_PNG) {
        return static_cast<std::uint16_t>(Win32ClipFormat::DibV5);
    }
    return std::nullopt;
}

auto mimes_from_win32_clip_formats(const std::vector<std::uint16_t> &formats) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const std::uint16_t format : formats) {
        const auto mime = mime_from_win32_clip_format(format);
        if (!mime.has_value()) {
            continue;  // 未登记格式：丢弃（不猜、不兜底成空串）
        }
        if (seen.insert(*mime).second) {
            out.push_back(*mime);
        }
    }
    return out;
}

auto win32_drop_effect_from_action(const DragAction action) -> std::uint32_t {
    switch (action) {
        case DragAction::Copy:
            return static_cast<std::uint32_t>(Win32DropEffect::Copy);
        case DragAction::Move:
            return static_cast<std::uint32_t>(Win32DropEffect::Move);
        case DragAction::Link:
            return static_cast<std::uint32_t>(Win32DropEffect::Link);
        case DragAction::None:
            break;
    }
    return static_cast<std::uint32_t>(Win32DropEffect::None);
}

auto action_from_win32_drop_effect(const std::uint32_t effect) -> DragAction {
    // 多位时按 Copy → Move → Link 取一（理由见头文件同名函数注记）。
    if ((effect & static_cast<std::uint32_t>(Win32DropEffect::Copy)) != 0U) {
        return DragAction::Copy;
    }
    if ((effect & static_cast<std::uint32_t>(Win32DropEffect::Move)) != 0U) {
        return DragAction::Move;
    }
    if ((effect & static_cast<std::uint32_t>(Win32DropEffect::Link)) != 0U) {
        return DragAction::Link;
    }
    return DragAction::None;
}

auto result_from_win32_drag_out(const Win32DragOutExit exit, const std::uint32_t effect) -> DragOutResult {
    DragOutResult result;
    switch (exit) {
        case Win32DragOutExit::Dropped:
            // 落放发生：效果位为零即目标侧「不接受」（DoDragDrop 契约：DROPEFFECT_NONE 表示
            // 没有目标接受该拖放）；非零则按 Copy → Move → Link 折取出真实动作。
            if (effect == 0U) {
                result.outcome = SystemDragOutcome::Rejected;
                result.action = DragAction::None;
            } else {
                result.outcome = SystemDragOutcome::Succeeded;
                result.action = action_from_win32_drop_effect(effect);
            }
            break;
        case Win32DragOutExit::Cancelled:
        case Win32DragOutExit::DefaultCursors:
            // 取消与「未发生落放」都是用户意图路径，不是故障：一律 Cancelled，不冒充失败。
            result.outcome = SystemDragOutcome::Cancelled;
            result.action = DragAction::None;
            break;
        case Win32DragOutExit::Unexpected:
            break;  // 异常终止：保持默认 Failed
    }
    return result;
}

auto logical_from_client_px(const int x, const int y, const float scale) -> Point {
    // scale ≤ 0 按 1.0 处理：宿主侧异常值不得把坐标算成 NaN / Inf（那会污染命中测试与布局）。
    const float divisor = scale > 0.0F ? scale : 1.0F;
    return Point{.x = static_cast<float>(x) / divisor, .y = static_cast<float>(y) / divisor};
}

namespace {

/// @brief X11 目标名登记表（MIME → 目标 atom 名；顺序即优先级，理由见头文件同名函数注记）。
struct X11TargetEntry {
    std::string_view mime;
    std::vector<std::string> names;
};

/// @brief 登记表单一真源（进程内只读静态；头文件两个 registered_* 查询函数即由它展平）。
[[nodiscard]] const std::vector<X11TargetEntry> &x11_target_table() {
    static const std::vector<X11TargetEntry> TABLE = {
        {.mime = AURORA_MIME_TEXT_PLAIN, .names = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}},
        {.mime = AURORA_MIME_URI_LIST, .names = {"text/uri-list"}},
        {.mime = AURORA_MIME_IMAGE_PNG, .names = {"image/png"}},
    };
    return TABLE;
}

/// @brief 展平后的目标名清单（保持表内顺序；install 期 intern 的唯一来源）。
[[nodiscard]] const std::vector<std::string> &x11_target_names_flat() {
    static const std::vector<std::string> FLAT = [] {
        std::vector<std::string> out;
        for (const X11TargetEntry &entry : x11_target_table()) {
            for (const std::string &name : entry.names) {
                out.push_back(name);
            }
        }
        return out;
    }();
    return FLAT;
}

/// @brief 动作 atom 名登记表（`DragAction` → atom 名；`Ask` / `Private` 不登记）。
[[nodiscard]] const std::vector<std::pair<DragAction, std::string_view>> &x11_action_table() {
    static const std::vector<std::pair<DragAction, std::string_view>> TABLE = {
        {DragAction::Copy, "XdndActionCopy"},
        {DragAction::Move, "XdndActionMove"},
        {DragAction::Link, "XdndActionLink"},
    };
    return TABLE;
}

/// @brief Wayland 线上 MIME 名登记表（规范 MIME → 线上名；顺序即优先级，理由见头文件同名函数注记）。
struct WaylandOfferEntry {
    std::string_view mime;
    std::vector<std::string> names;
};

/// @brief 登记表单一真源（头文件 `wayland_registered_offer_names` 即由它展平）。
[[nodiscard]] const std::vector<WaylandOfferEntry> &wayland_offer_table() {
    static const std::vector<WaylandOfferEntry> TABLE = {
        // 互操作惯例：GTK/Qt/文件管理器三者之一给带 charset 的形或裸形，源侧两枚都广告。
        {.mime = AURORA_MIME_TEXT_PLAIN, .names = {"text/plain;charset=utf-8", "text/plain"}},
        {.mime = AURORA_MIME_URI_LIST, .names = {"text/uri-list"}},
        {.mime = AURORA_MIME_IMAGE_PNG, .names = {"image/png"}},
    };
    return TABLE;
}

}  // namespace

auto x11_target_names_from_mime(const std::string_view mime) -> std::vector<std::string> {
    for (const X11TargetEntry &entry : x11_target_table()) {
        if (entry.mime == mime) {
            return entry.names;
        }
    }
    return {};  // 未登记 MIME：不猜、不兜底（与 Win32 数值表同纪律）
}

auto mime_from_x11_target_name(const std::string_view name) -> std::optional<std::string> {
    for (const X11TargetEntry &entry : x11_target_table()) {
        for (const std::string &candidate : entry.names) {
            if (candidate == name) {
                return std::string{entry.mime};
            }
        }
    }
    return std::nullopt;
}

auto mimes_from_x11_target_names(const std::vector<std::string> &names) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const std::string &name : names) {
        const auto mime = mime_from_x11_target_name(name);
        if (!mime.has_value()) {
            continue;  // 未登记目标：丢弃（不猜、不兜底成空串）
        }
        if (seen.insert(*mime).second) {
            out.push_back(*mime);
        }
    }
    return out;
}

auto x11_action_from_name(const std::string_view name) -> DragAction {
    for (const auto &[action, atom_name] : x11_action_table()) {
        if (atom_name == name) {
            return action;
        }
    }
    // XdndActionAsk / XdndActionPrivate / 未知名：本库无对应态，如实为 None（见头文件注记）。
    return DragAction::None;
}

auto x11_action_name_from(const DragAction action) -> std::string {
    for (const auto &[mapped, atom_name] : x11_action_table()) {
        if (mapped == action) {
            return std::string{atom_name};
        }
    }
    return {};  // None：调用方直接用 atom None，不 intern 空名
}

auto x11_registered_target_names() -> const std::vector<std::string> & { return x11_target_names_flat(); }

auto x11_registered_action_names() -> const std::vector<std::string> & {
    static const std::vector<std::string> FLAT = [] {
        std::vector<std::string> out;
        for (const auto &[action, atom_name] : x11_action_table()) {
            (void)action;
            out.emplace_back(atom_name);
        }
        return out;
    }();
    return FLAT;
}

auto wayland_supported_dnd_actions() -> std::uint32_t {
    return static_cast<std::uint32_t>(WaylandDndAction::Copy) | static_cast<std::uint32_t>(WaylandDndAction::Move);
}

auto wayland_dnd_action_from(const DragAction action) -> std::uint32_t {
    for (const auto &[mapped, bit] : std::array<std::pair<DragAction, std::uint32_t>, 2>{
             std::pair<DragAction, std::uint32_t>{DragAction::Copy, static_cast<std::uint32_t>(WaylandDndAction::Copy)},
             std::pair<DragAction, std::uint32_t>{DragAction::Move, static_cast<std::uint32_t>(WaylandDndAction::Move)},
         }) {
        if (mapped == action) {
            return bit;
        }
    }
    return static_cast<std::uint32_t>(WaylandDndAction::None);  // None / Link：桥不声明
}

auto action_from_wayland_dnd_action(const std::uint32_t bits) -> DragAction {
    // 多位时按 Copy → Move → Link 取一（与 Win32 侧同名函数同形：桥不声明 Link 位）。
    if ((bits & static_cast<std::uint32_t>(WaylandDndAction::Copy)) != 0U) {
        return DragAction::Copy;
    }
    if ((bits & static_cast<std::uint32_t>(WaylandDndAction::Move)) != 0U) {
        return DragAction::Move;
    }
    // Ask 位与未知位：本库无对应态，如实为 None（见头文件注记）。
    return DragAction::None;
}

auto wayland_offer_names_from_mime(const std::string_view mime) -> std::vector<std::string> {
    for (const WaylandOfferEntry &entry : wayland_offer_table()) {
        if (entry.mime == mime) {
            return entry.names;
        }
    }
    return {};  // 未登记 MIME：不猜、不兜底（与 X11 / Win32 表同纪律）
}

auto mime_from_wayland_offer_name(const std::string_view name) -> std::optional<std::string> {
    // 先按字面量全表比对（登记表内的形一律字面命中）。
    for (const WaylandOfferEntry &entry : wayland_offer_table()) {
        for (const std::string &candidate : entry.names) {
            if (candidate == name) {
                return std::string{entry.mime};
            }
        }
    }
    // 未字面命中时只对 text/plain 放宽：剥掉 `;` 后的参数再比一次。对端文件管理器给的参数集合
    // 不可控（`charset=`、`encoding=` 都可能出现），按字面量比较会把同一格式判成两个格式。
    // 其余 MIME 不放宽——参数化的 `image/png;foo` 之类没有互操作先例，放宽只会放大误判面。
    if (const auto sep = name.find(';'); sep != std::string_view::npos) {
        const std::string_view base = name.substr(0, sep);
        if (base == AURORA_MIME_TEXT_PLAIN) {
            return std::string{AURORA_MIME_TEXT_PLAIN};
        }
    }
    return std::nullopt;
}

auto mimes_from_wayland_offer_names(const std::vector<std::string> &names) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const std::string &name : names) {
        const auto mime = mime_from_wayland_offer_name(name);
        if (!mime.has_value()) {
            continue;  // 未登记名：丢弃（不猜、不兜底成空串）
        }
        if (seen.insert(*mime).second) {
            out.push_back(*mime);
        }
    }
    return out;
}

auto wayland_registered_offer_names() -> const std::vector<std::string> & {
    static const std::vector<std::string> FLAT = [] {
        std::vector<std::string> out;
        for (const WaylandOfferEntry &entry : wayland_offer_table()) {
            for (const std::string &name : entry.names) {
                out.push_back(name);
            }
        }
        return out;
    }();
    return FLAT;
}

auto make_drop_event(const SystemDropPhase phase, const DragInFrame &frame) -> SystemDropEvent {
    SystemDropEvent event;
    event.phase = phase;
    event.position = logical_from_client_px(frame.client_x, frame.client_y, frame.scale);
    // 二选一（DragInFrame::mimes 注记）：X11 通道直接采用折算好的 MIME 清单，Win32 通道过数值表。
    event.formats = frame.mimes.empty() ? mimes_from_win32_clip_formats(frame.clip_formats) : frame.mimes;
    event.modifiers = frame.modifiers;
    // 拖入侧的缺省动作即 OS 报告值（DragInFrame::os_suggested 注记）；OS 未报告（None）时
    // proposed_action_for 自身按 Copy 兜底——两路兜底只此一处，不另写。翻译点唯一：
    // 修饰键 → 动作的翻译全库只有 system_drag::proposed_action_for 这一处，折算层复用而非重写。
    event.proposed = system_drag::proposed_action_for(frame.modifiers, frame.os_suggested);
    return event;
}

}  // namespace aurora::drag
