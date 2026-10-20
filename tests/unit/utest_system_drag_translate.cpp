/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/system_drag_translate.h
/// 测试说明: 系统拖放的**零平台头折算**——客户区像素→逻辑 dp（含 DPI 与非正缩放防御）、
/// Win32 CLIPFORMAT ↔ MIME 双向映射与清单（保序去重、未登记丢弃）、MK_* → ModifierKey（Meta
/// 恒不置位、未知位忽略）、DragAction ↔ DROPEFFECT 双向（多位按 Copy→Move→Link 取一）、
/// 拖入侧 proposed 的来源（OS 报告值优先、修饰键覆盖、锁定态位不污染、OS 缺席按 Copy 兜底）、
/// 同步载荷阈值常量锚定、make_drop_event 的字段装配。
///
/// 变异自证（改坏实现应转红的三条）：
///  * 摘掉 `proposed_action_for` 的锁定态位掩码 → `..._ignores_lock_bits` 转红；
///  * 把「OS 缺席兜底」从 Copy 改成 None → `..._falls_back_to_copy_when_os_silent` 转红；
///  * 把阈值常量改成各平台桥自持的散数 → `sync_payload_limit_is_anchored` 转红（跨桥一致性
///    另由「四桥均引用该常量」的静态判据钉住，不属本文件职责）。

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/window/detail/system_drag_translate.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_system_drag_translate {

using aurora::drag::action_from_win32_drop_effect;
using aurora::drag::AURORA_DRAG_SYNC_PAYLOAD_LIMIT;
using aurora::drag::DragInFrame;
using aurora::drag::logical_from_client_px;
using aurora::drag::make_drop_event;
using aurora::drag::mime_from_win32_clip_format;
using aurora::drag::mimes_from_win32_clip_formats;
using aurora::drag::modifiers_from_win32_mk_flags;
using aurora::drag::win32_clip_format_from_mime;
using aurora::drag::win32_drop_effect_from_action;
using aurora::drag::Win32ClipFormat;
using aurora::drag::Win32DropEffect;
using aurora::drag::Win32MkFlag;

namespace {

constexpr std::uint16_t AURORA_CF_TEXT = static_cast<std::uint16_t>(Win32ClipFormat::UnicodeText);
constexpr std::uint16_t AURORA_CF_HDROP = static_cast<std::uint16_t>(Win32ClipFormat::Hdrop);
constexpr std::uint16_t AURORA_CF_DIBV5 = static_cast<std::uint16_t>(Win32ClipFormat::DibV5);
constexpr std::uint32_t AURORA_MK_SHIFT = static_cast<std::uint32_t>(Win32MkFlag::Shift);
constexpr std::uint32_t AURORA_MK_CONTROL = static_cast<std::uint32_t>(Win32MkFlag::Control);
constexpr std::uint32_t AURORA_MK_ALT = static_cast<std::uint32_t>(Win32MkFlag::Alt);
constexpr std::uint32_t AURORA_EFFECT_COPY = static_cast<std::uint32_t>(Win32DropEffect::Copy);
constexpr std::uint32_t AURORA_EFFECT_MOVE = static_cast<std::uint32_t>(Win32DropEffect::Move);
constexpr std::uint32_t AURORA_EFFECT_LINK = static_cast<std::uint32_t>(Win32DropEffect::Link);

/// @brief 取位集里的某一位是否置位（ModifierKey 是位掩码枚举，逐位比较须显式转换）。
[[nodiscard]] auto has_mod(ModifierKey mods, ModifierKey bit) -> bool {
    return (static_cast<std::uint8_t>(mods) & static_cast<std::uint8_t>(bit)) != 0U;
}

/// @brief 默认帧：客户区 (100, 200)、scale 2.0、无修饰键、OS 未报告动作、无格式。
[[nodiscard]] auto plain_frame() -> DragInFrame {
    DragInFrame frame;
    frame.client_x = 100;
    frame.client_y = 200;
    frame.scale = 2.0F;
    return frame;
}

}  // namespace

AURORA_TEST_CASE(translate_logical_coords_divide_by_scale) {
    const Point a = logical_from_client_px(100, 200, 1.0F);
    AURORA_TEST_CHECK_NEAR(a.x, 100.0F, 0.001F);
    AURORA_TEST_CHECK_NEAR(a.y, 200.0F, 0.001F);

    const Point b = logical_from_client_px(100, 200, 2.0F);
    AURORA_TEST_CHECK_NEAR(b.x, 50.0F, 0.001F);
    AURORA_TEST_CHECK_NEAR(b.y, 100.0F, 0.001F);

    // 150% 缩放：分数结果不得被截断成整数（截断会让命中测试在缩放屏上系统性偏移）。
    const Point c = logical_from_client_px(100, 200, 1.5F);
    AURORA_TEST_CHECK_NEAR(c.x, 66.66667F, 0.001F);
    AURORA_TEST_CHECK_NEAR(c.y, 133.33333F, 0.001F);
}

AURORA_TEST_CASE(translate_logical_coords_survive_non_positive_scale) {
    // 宿主给出异常缩放（0 / 负值）时不得产出 NaN 或 Inf：那会污染命中测试与布局且极难定位。
    for (const float bad : {0.0F, -1.0F, -2.5F}) {
        const Point p = logical_from_client_px(30, 40, bad);
        AURORA_TEST_CHECK_FALSE(std::isnan(p.x));
        AURORA_TEST_CHECK_FALSE(std::isnan(p.y));
        AURORA_TEST_CHECK_FALSE(std::isinf(p.x));
        AURORA_TEST_CHECK_FALSE(std::isinf(p.y));
        AURORA_TEST_CHECK_NEAR(p.x, 30.0F, 0.001F);
        AURORA_TEST_CHECK_NEAR(p.y, 40.0F, 0.001F);
    }
}

AURORA_TEST_CASE(translate_clip_format_maps_registered_formats_to_mime) {
    AURORA_TEST_CHECK_STREQ(mime_from_win32_clip_format(AURORA_CF_TEXT).value_or("<none>"), "text/plain");
    AURORA_TEST_CHECK_STREQ(mime_from_win32_clip_format(AURORA_CF_HDROP).value_or("<none>"), "text/uri-list");
    AURORA_TEST_CHECK_STREQ(mime_from_win32_clip_format(AURORA_CF_DIBV5).value_or("<none>"), "image/png");
}

AURORA_TEST_CASE(translate_clip_format_rejects_unregistered_formats) {
    // 未登记格式按「不认」处理：猜一个 MIME 会让控件层的 accept 查询基于假格式判真。
    AURORA_TEST_CHECK_FALSE(mime_from_win32_clip_format(1U).has_value());  // CF_TEXT（ANSI 变体）
    AURORA_TEST_CHECK_FALSE(mime_from_win32_clip_format(8U).has_value());  // CF_BITMAP
    AURORA_TEST_CHECK_FALSE(win32_clip_format_from_mime("application/x-custom").has_value());
    AURORA_TEST_CHECK_FALSE(win32_clip_format_from_mime("").has_value());
}

AURORA_TEST_CASE(translate_mime_list_preserves_priority_and_dedups) {
    // 顺序即源侧声明的优先级：折算不得重排（重排会让「首选格式」在不同平台上不一致）。
    const std::vector<std::uint16_t> formats{AURORA_CF_DIBV5, AURORA_CF_TEXT, AURORA_CF_DIBV5, 8U, AURORA_CF_HDROP};
    const std::vector<std::string> mimes = mimes_from_win32_clip_formats(formats);
    AURORA_TEST_REQUIRE_EQ(mimes.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(mimes[0], "image/png");
    AURORA_TEST_CHECK_STREQ(mimes[1], "text/plain");
    AURORA_TEST_CHECK_STREQ(mimes[2], "text/uri-list");

    AURORA_TEST_CHECK_TRUE(mimes_from_win32_clip_formats({}).empty());
    AURORA_TEST_CHECK_TRUE(mimes_from_win32_clip_formats({8U, 9U}).empty());
}

AURORA_TEST_CASE(translate_mime_to_clip_format_round_trips) {
    for (const std::uint16_t format : {AURORA_CF_TEXT, AURORA_CF_HDROP, AURORA_CF_DIBV5}) {
        const auto mime = mime_from_win32_clip_format(format);
        AURORA_TEST_REQUIRE_TRUE(mime.has_value());
        const auto back = win32_clip_format_from_mime(*mime);
        AURORA_TEST_REQUIRE_TRUE(back.has_value());
        AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(*back), static_cast<std::uint32_t>(format));
    }
}

AURORA_TEST_CASE(translate_mk_flags_cover_pressable_bits_only) {
    const ModifierKey none = modifiers_from_win32_mk_flags(0U);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(none), 0U);

    const ModifierKey combo = modifiers_from_win32_mk_flags(AURORA_MK_SHIFT | AURORA_MK_CONTROL | AURORA_MK_ALT);
    AURORA_TEST_CHECK_TRUE(has_mod(combo, ModifierKey::Shift));
    AURORA_TEST_CHECK_TRUE(has_mod(combo, ModifierKey::Control));
    AURORA_TEST_CHECK_TRUE(has_mod(combo, ModifierKey::Alt));
    // Win 键在 MK_* 位集里无从表达：如实不置位，不猜。
    AURORA_TEST_CHECK_FALSE(has_mod(combo, ModifierKey::Meta));
    // MK_* 不含锁定态：折算结果不得带出 NumLock。
    AURORA_TEST_CHECK_FALSE(has_mod(combo, ModifierKey::NumLock));

    // 未知位（鼠标键位等）一律忽略，不进修饰位集。
    const ModifierKey with_mouse = modifiers_from_win32_mk_flags(AURORA_MK_CONTROL | 0x0001U);
    AURORA_TEST_CHECK_TRUE(has_mod(with_mouse, ModifierKey::Control));
    AURORA_TEST_CHECK_FALSE(has_mod(with_mouse, ModifierKey::Shift));
}

AURORA_TEST_CASE(translate_drop_effect_maps_actions_both_ways) {
    AURORA_TEST_CHECK_EQ(win32_drop_effect_from_action(DragAction::Copy), AURORA_EFFECT_COPY);
    AURORA_TEST_CHECK_EQ(win32_drop_effect_from_action(DragAction::Move), AURORA_EFFECT_MOVE);
    AURORA_TEST_CHECK_EQ(win32_drop_effect_from_action(DragAction::Link), AURORA_EFFECT_LINK);
    AURORA_TEST_CHECK_EQ(win32_drop_effect_from_action(DragAction::None), std::uint32_t{0});

    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(AURORA_EFFECT_COPY) == DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(AURORA_EFFECT_MOVE) == DragAction::Move);
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(AURORA_EFFECT_LINK) == DragAction::Link);
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(0U) == DragAction::None);
    // 未知位（DROPEFFECT_SCROLL 一类）忽略，不误判成某个已知动作。
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(0x80000000U) == DragAction::None);
}

AURORA_TEST_CASE(translate_drop_effect_prefers_copy_when_multi_bit) {
    // OS 可同时声明多个位（源侧「可复制亦可移动」）：按 Copy → Move → Link 取一，
    // 与 proposed_action_for 的「无修饰键按缺省 Copy」同源。
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(AURORA_EFFECT_MOVE | AURORA_EFFECT_COPY) == DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_win32_drop_effect(AURORA_EFFECT_LINK | AURORA_EFFECT_MOVE) == DragAction::Move);
}

AURORA_TEST_CASE(translate_drop_in_proposed_takes_os_suggestion_when_no_modifiers) {
    // 拖入侧没有 DragOffer：缺省动作只能取 OS 报告值。
    DragInFrame frame = plain_frame();
    frame.os_suggested = DragAction::Move;
    const SystemDropEvent event = make_drop_event(SystemDropPhase::Enter, frame);
    AURORA_TEST_CHECK_TRUE(event.proposed == DragAction::Move);
}

AURORA_TEST_CASE(translate_drop_in_falls_back_to_copy_when_os_silent) {
    // OS 未报告（None）且无修饰键 → Copy 兜底（与 proposed_action_for 的既有兜底同源，唯一处）。
    const SystemDropEvent event = make_drop_event(SystemDropPhase::Enter, plain_frame());
    AURORA_TEST_CHECK_TRUE(event.proposed == DragAction::Copy);
}

AURORA_TEST_CASE(translate_drop_in_modifiers_override_os_suggestion) {
    // 修饰键是用户此刻的显式意图，压过 OS 报告值（Windows 惯例）。
    DragInFrame ctrl = plain_frame();
    ctrl.modifiers = ModifierKey::Control;
    ctrl.os_suggested = DragAction::Move;
    AURORA_TEST_CHECK_TRUE(make_drop_event(SystemDropPhase::Over, ctrl).proposed == DragAction::Copy);

    DragInFrame shift = plain_frame();
    shift.modifiers = ModifierKey::Shift;
    shift.os_suggested = DragAction::Copy;
    AURORA_TEST_CHECK_TRUE(make_drop_event(SystemDropPhase::Over, shift).proposed == DragAction::Move);

    DragInFrame both = plain_frame();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位或，结果为合法组合值
    both.modifiers = static_cast<ModifierKey>(static_cast<std::uint8_t>(ModifierKey::Control) |
                                              static_cast<std::uint8_t>(ModifierKey::Shift));
    AURORA_TEST_CHECK_TRUE(make_drop_event(SystemDropPhase::Over, both).proposed == DragAction::Link);
}

AURORA_TEST_CASE(translate_drop_in_proposed_ignores_lock_bits) {
    // 锁定态位（NumLock 常开）不得污染动作判据：否则 Ctrl 组合在 NumLock 开启的机器上静默失配。
    DragInFrame frame = plain_frame();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) 位掩码枚举按位或，结果为合法组合值
    frame.modifiers = static_cast<ModifierKey>(static_cast<std::uint8_t>(ModifierKey::Control) |
                                               static_cast<std::uint8_t>(ModifierKey::NumLock));
    const SystemDropEvent event = make_drop_event(SystemDropPhase::Over, frame);
    AURORA_TEST_CHECK_TRUE(event.proposed == DragAction::Copy);
}

AURORA_TEST_CASE(translate_make_drop_event_assembles_contract_fields) {
    DragInFrame frame = plain_frame();
    frame.clip_formats = {AURORA_CF_HDROP, AURORA_CF_TEXT};
    frame.modifiers = ModifierKey::Shift;
    frame.os_suggested = DragAction::Copy;

    const SystemDropEvent event = make_drop_event(SystemDropPhase::Drop, frame);
    AURORA_TEST_CHECK_TRUE(event.phase == SystemDropPhase::Drop);
    AURORA_TEST_CHECK_NEAR(event.position.x, 50.0F, 0.001F);
    AURORA_TEST_CHECK_NEAR(event.position.y, 100.0F, 0.001F);
    AURORA_TEST_REQUIRE_EQ(event.formats.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(event.formats[0], "text/uri-list");
    AURORA_TEST_CHECK_STREQ(event.formats[1], "text/plain");
    AURORA_TEST_CHECK_TRUE(event.proposed == DragAction::Move);
    // allowed 由派发器写定、payload 由调用方装配：折算层两处都不碰。
    AURORA_TEST_CHECK_TRUE(event.allowed == DragAction::None);
    AURORA_TEST_CHECK_TRUE(event.payload == nullptr);
}

AURORA_TEST_CASE(translate_sync_payload_limit_is_anchored) {
    // 阈值是四个平台桥共用的单一真源：改常量本例即红，防止「某个桥另写一个数」的分叉。
    AURORA_TEST_CHECK_EQ(AURORA_DRAG_SYNC_PAYLOAD_LIMIT, std::size_t{65536});
    AURORA_TEST_CHECK_GT(AURORA_DRAG_SYNC_PAYLOAD_LIMIT, std::size_t{0});
}

}  // namespace aurora::test_cases::utest_system_drag_translate
