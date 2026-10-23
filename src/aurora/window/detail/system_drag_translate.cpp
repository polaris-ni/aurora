// 系统拖放折算层实现：**零平台头**（契约与理由见同名头文件）。
//
// 本 TU 不引任何平台头：全部入参都是纯值（数值 / 枚举 / 字符串），故它在每个 CI 作业上都能编译，
// 单测也就能在任何平台上覆盖全部折算逻辑。

#include "system_drag_translate.h"

#include <algorithm>
#include <unordered_set>

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

auto make_drop_event(const SystemDropPhase phase, const DragInFrame &frame) -> SystemDropEvent {
    SystemDropEvent event;
    event.phase = phase;
    event.position = logical_from_client_px(frame.client_x, frame.client_y, frame.scale);
    event.formats = mimes_from_win32_clip_formats(frame.clip_formats);
    event.modifiers = frame.modifiers;
    // 拖入侧的缺省动作即 OS 报告值（DragInFrame::os_suggested 注记）；OS 未报告（None）时
    // proposed_action_for 自身按 Copy 兜底——两路兜底只此一处，不另写。翻译点唯一：
    // 修饰键 → 动作的翻译全库只有 system_drag::proposed_action_for 这一处，折算层复用而非重写。
    event.proposed = system_drag::proposed_action_for(frame.modifiers, frame.os_suggested);
    return event;
}

}  // namespace aurora::drag
