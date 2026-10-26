/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/system_drag_translate.h
/// 测试说明: 系统拖放的**零平台头折算**——客户区像素→逻辑 dp（含 DPI 与非正缩放防御）、
/// Win32 CLIPFORMAT ↔ MIME 双向映射与清单（保序去重、未登记丢弃）、MK_* → ModifierKey（Meta
/// 恒不置位、未知位忽略）、DragAction ↔ DROPEFFECT 双向（多位按 Copy→Move→Link 取一）、
/// X11 选择目标名 ↔ MIME（UTF-8 三别名、latin1 的 STRING/TEXT 不登记）、`XdndAction*` ↔
/// DragAction（Ask/Private → None）、Wayland 线上 MIME 名 ↔ 规范 MIME（`text/plain` 参数剥离、
/// 进程内 `aurora/widget` 不登记）与 `dnd_action` 位 ↔ DragAction（Copy→Move 优先序、Ask → None）、
/// 拖入侧 proposed 的来源（OS 报告值优先、修饰键覆盖、锁定态位不污染、OS 缺席按 Copy 兜底）、
/// 同步载荷阈值常量锚定、make_drop_event 的字段装配（含 X11 的 mimes 通道优先于 Win32 数值通道）。
///
/// 变异自证（改坏实现应转红的三条）：
///  * 摘掉 `proposed_action_for` 的锁定态位掩码 → `..._ignores_lock_bits` 转红；
///  * 把「OS 缺席兜底」从 Copy 改成 None → `..._falls_back_to_copy_when_os_silent` 转红；
///  * 把阈值常量改成各平台桥自持的散数 → `sync_payload_limit_is_anchored` 转红（跨桥一致性
///    另由「四桥均引用该常量」的静态判据钉住，不属本文件职责）；
///  * 把 latin1 的 `STRING` 登记进目标表 → `..._reverse_maps_aliases_but_not_latin1` 转红
///    （登记它会让「读回字节按 UTF-8 解码」静默乱码）；
///  * 让 `mimes` 与 `clip_formats` 双折算（而非二选一）→ `..._mimes_channel_takes_precedence` 转红；
///  * 把 Wayland 的 `text/plain;charset=utf-8` 按字面量比较（不剥参数）→
///    `..._reverse_strips_parameters_for_text_plain` 转红（同一格式被对端判成两个）；
///  * 把 `aurora/widget` 登记进 Wayland 线上名表 → `..._offer_names_map_registered_mime` 转红
///    （进程内词汇不该出现在跨进程协议上）；
///  * 把 legacy `NSStringPboardType` 从 macOS text/plain 表里删掉 → `..._reverse_maps_registered_types`
///    转红（旧应用仍写它，删了即静默读不到）；
///  * 把 `NSFilenamesPboardType` 登记进 macOS uri-list 表 → 同例转红（其载荷是路径串数组而非
///    URI 清单，登记即声称能读一种解码不了的形态）；
///  * 让 macOS 动作折算复用 Win32 的 `DROPEFFECT` 位序 → `..._drag_operation_bits_map_both_ways`
///    转红（Link=2 而 Move=16，位序三平台两样）。

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "aurora/app/mime_types.h"
#include "aurora/event/event.h"
#include "aurora/window/detail/system_drag_translate.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_system_drag_translate {

using aurora::drag::action_from_macos_drag_operation;
using aurora::drag::action_from_wayland_dnd_action;
using aurora::drag::action_from_win32_drop_effect;
using aurora::drag::AURORA_DRAG_SYNC_PAYLOAD_LIMIT;
using aurora::drag::DragInFrame;
using aurora::drag::logical_from_client_px;
using aurora::drag::macos_drag_operation_from;
using aurora::drag::macos_pasteboard_types_from_mime;
using aurora::drag::macos_registered_pasteboard_types;
using aurora::drag::macos_supported_drag_operations;
using aurora::drag::MacOSDragOperation;
using aurora::drag::make_drop_event;
using aurora::drag::mime_from_macos_pasteboard_type;
using aurora::drag::mime_from_wayland_offer_name;
using aurora::drag::mime_from_win32_clip_format;
using aurora::drag::mime_from_x11_target_name;
using aurora::drag::mimes_from_macos_pasteboard_types;
using aurora::drag::mimes_from_wayland_offer_names;
using aurora::drag::mimes_from_win32_clip_formats;
using aurora::drag::mimes_from_x11_target_names;
using aurora::drag::modifiers_from_win32_mk_flags;
using aurora::drag::result_from_win32_drag_out;
using aurora::drag::wayland_dnd_action_from;
using aurora::drag::wayland_offer_names_from_mime;
using aurora::drag::wayland_registered_offer_names;
using aurora::drag::wayland_supported_dnd_actions;
using aurora::drag::WaylandDndAction;
using aurora::drag::win32_clip_format_from_mime;
using aurora::drag::win32_drop_effect_from_action;
using aurora::drag::Win32ClipFormat;
using aurora::drag::Win32DragOutExit;
using aurora::drag::Win32DropEffect;
using aurora::drag::Win32MkFlag;
using aurora::drag::x11_action_from_name;
using aurora::drag::x11_action_name_from;
using aurora::drag::x11_registered_action_names;
using aurora::drag::x11_registered_target_names;
using aurora::drag::x11_target_names_from_mime;
using aurora::testing::require_value;

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
constexpr auto AURORA_EXIT_DROPPED = Win32DragOutExit::Dropped;
constexpr auto AURORA_EXIT_CANCELLED = Win32DragOutExit::Cancelled;
constexpr auto AURORA_EXIT_DEFAULT_CURSORS = Win32DragOutExit::DefaultCursors;
constexpr auto AURORA_EXIT_UNEXPECTED = Win32DragOutExit::Unexpected;

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
        const std::string mime = require_value(mime_from_win32_clip_format(format));
        const auto back = win32_clip_format_from_mime(mime);
        AURORA_TEST_CHECK_EQ(static_cast<std::uint32_t>(require_value(back)), static_cast<std::uint32_t>(format));
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

// ---- X11 Xdnd 侧：选择目标名 ↔ MIME、动作 atom 名 ↔ DragAction ----

AURORA_TEST_CASE(translate_x11_target_names_map_registered_mime_to_utf8_aliases) {
    // text/plain 挂三个互操作别名，且**首选别名是带字符集的那一个**（advertise 与 request 都取
    // 首个可用项）：GTK / Qt 源三者之一都会出现，缺一即与常见源失联。
    const std::vector<std::string> text = x11_target_names_from_mime(AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_REQUIRE_EQ(text.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(text[0], "text/plain;charset=utf-8");
    AURORA_TEST_CHECK_STREQ(text[1], "UTF8_STRING");
    AURORA_TEST_CHECK_STREQ(text[2], "text/plain");

    const std::vector<std::string> uris = x11_target_names_from_mime(AURORA_MIME_URI_LIST);
    AURORA_TEST_REQUIRE_EQ(uris.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(uris[0], "text/uri-list");

    const std::vector<std::string> png = x11_target_names_from_mime(AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_REQUIRE_EQ(png.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(png[0], "image/png");

    // 未登记 MIME 不映射（与 Win32 数值表同纪律：不猜、不兜底成空串）。
    AURORA_TEST_CHECK_TRUE(x11_target_names_from_mime("application/x-custom").empty());
    AURORA_TEST_CHECK_TRUE(x11_target_names_from_mime("").empty());
}

AURORA_TEST_CASE(translate_x11_target_name_reverse_maps_aliases_but_not_latin1) {
    for (const char *name : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) {
        const auto mime = mime_from_x11_target_name(name);
        AURORA_TEST_REQUIRE_TRUE(mime.has_value());
        AURORA_TEST_CHECK_STREQ(require_value(mime), AURORA_MIME_TEXT_PLAIN);
    }
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_x11_target_name("text/uri-list")), AURORA_MIME_URI_LIST);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_x11_target_name("image/png")), AURORA_MIME_IMAGE_PNG);

    // 纯 latin1 的 `STRING` / `TEXT` **不登记**：本库文本契约是 UTF-8，登记 latin1 目标会让
    // 「读回的字节按 UTF-8 解码」静默乱码。宁可不认该格式，也不产出错误的解码。
    AURORA_TEST_CHECK_FALSE(mime_from_x11_target_name("STRING").has_value());
    AURORA_TEST_CHECK_FALSE(mime_from_x11_target_name("TEXT").has_value());
    AURORA_TEST_CHECK_FALSE(mime_from_x11_target_name("").has_value());
}

AURORA_TEST_CASE(translate_x11_mime_list_preserves_priority_and_dedups) {
    // 顺序即优先级（源侧声明序），折算不得重排；未登记目标丢弃。
    const std::vector<std::string> names{"image/png", "UTF8_STRING",   "text/plain",
                                         "STRING",    "text/uri-list", "image/png"};
    const std::vector<std::string> mimes = mimes_from_x11_target_names(names);
    AURORA_TEST_REQUIRE_EQ(mimes.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(mimes[0], AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_CHECK_STREQ(mimes[1], AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(mimes[2], AURORA_MIME_URI_LIST);

    AURORA_TEST_CHECK_TRUE(mimes_from_x11_target_names({}).empty());
    AURORA_TEST_CHECK_TRUE(mimes_from_x11_target_names({"STRING", "TEXT"}).empty());
}

AURORA_TEST_CASE(translate_x11_action_names_map_both_ways) {
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionCopy") == DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionMove") == DragAction::Move);
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionLink") == DragAction::Link);
    AURORA_TEST_CHECK_STREQ(x11_action_name_from(DragAction::Copy), "XdndActionCopy");
    AURORA_TEST_CHECK_STREQ(x11_action_name_from(DragAction::Move), "XdndActionMove");
    AURORA_TEST_CHECK_STREQ(x11_action_name_from(DragAction::Link), "XdndActionLink");

    for (const DragAction action : {DragAction::Copy, DragAction::Move, DragAction::Link}) {
        AURORA_TEST_CHECK_TRUE(x11_action_from_name(x11_action_name_from(action)) == action);
    }
}

AURORA_TEST_CASE(translate_x11_action_names_reject_ask_private_and_unknown) {
    // `Ask` / `Private` 与未知名一律 None：本库的 DragAction 没有「询问用户」与「应用私有」两态，
    // 如实映射为「无动作」而非替用户猜一个——光标反馈与动作协商都据此收车。
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionAsk") == DragAction::None);
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionPrivate") == DragAction::None);
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("XdndActionNothing") == DragAction::None);  // 未知名
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("UTF8_STRING") == DragAction::None);  // 张冠不可李戴
}

AURORA_TEST_CASE(translate_x11_action_name_from_none_is_empty) {
    // None → 空串：调用方据此直接用 atom `None`，不去 intern 一个空名（XInternAtom("") 无意义）。
    AURORA_TEST_CHECK_STREQ(x11_action_name_from(DragAction::None), "");
    AURORA_TEST_CHECK_TRUE(x11_action_from_name("") == DragAction::None);
}

AURORA_TEST_CASE(translate_x11_registered_lists_are_the_single_source) {
    // 两张登记表由折算层给出、桥在 install 期按它 intern：这里钉住「登记表即映射表的展平」，
    // 防止桥里另抄一份名字（抄错一个字母即与对端失联，且无头测不到）。
    const std::vector<std::string> &names = x11_registered_target_names();
    AURORA_TEST_REQUIRE_EQ(names.size(), std::size_t{5});
    for (const std::string &name : names) {
        AURORA_TEST_CHECK_TRUE(mime_from_x11_target_name(name).has_value());
    }
    const std::vector<std::string> &actions = x11_registered_action_names();
    AURORA_TEST_REQUIRE_EQ(actions.size(), std::size_t{3});
    for (const std::string &name : actions) {
        AURORA_TEST_CHECK_FALSE(x11_action_from_name(name) == DragAction::None);
    }
}

AURORA_TEST_CASE(translate_drop_in_mimes_channel_takes_precedence_over_clip_formats) {
    // 二选一而非并存：X11 通道填 mimes（目标名已折算），再过一遍 Win32 数值表等于把 MIME 塞回
    // 数值再折回来——无意义的往返会把未登记 MIME 悄悄吞掉。
    DragInFrame frame = plain_frame();
    frame.mimes = {std::string{AURORA_MIME_TEXT_PLAIN}, std::string{AURORA_MIME_IMAGE_PNG}};
    frame.clip_formats = {AURORA_CF_HDROP};  // 干扰项：mimes 非空时必须被忽略

    const SystemDropEvent event = make_drop_event(SystemDropPhase::Over, frame);
    AURORA_TEST_REQUIRE_EQ(event.formats.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(event.formats[0], AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(event.formats[1], AURORA_MIME_IMAGE_PNG);

    // mimes 为空（Win32 通道）时落回数值表。
    DragInFrame win32 = plain_frame();
    win32.clip_formats = {AURORA_CF_HDROP};
    const SystemDropEvent fallback = make_drop_event(SystemDropPhase::Over, win32);
    AURORA_TEST_REQUIRE_EQ(fallback.formats.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(fallback.formats[0], AURORA_MIME_URI_LIST);
}

// ---- 拖出侧：`DoDragDrop` 退出码 + 效果位 → 会话终态 ----

AURORA_TEST_CASE(translate_drag_out_dropped_with_effect_succeeds) {
    // 落放发生且效果位非零：Succeeded + 效果位折取的动作（Copy → Move → Link）。
    const auto copy = result_from_win32_drag_out(AURORA_EXIT_DROPPED, AURORA_EFFECT_COPY);
    AURORA_TEST_CHECK_TRUE(copy.outcome == SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_TRUE(copy.action == DragAction::Copy);

    const auto move = result_from_win32_drag_out(AURORA_EXIT_DROPPED, AURORA_EFFECT_MOVE);
    AURORA_TEST_CHECK_TRUE(move.outcome == SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_TRUE(move.action == DragAction::Move);

    const auto link = result_from_win32_drag_out(AURORA_EXIT_DROPPED, AURORA_EFFECT_LINK);
    AURORA_TEST_CHECK_TRUE(link.outcome == SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_TRUE(link.action == DragAction::Link);

    // 多位并存时与拖入侧同序取一（Copy 优先），两条通道口径一致。
    const auto multi = result_from_win32_drag_out(AURORA_EXIT_DROPPED, AURORA_EFFECT_MOVE | AURORA_EFFECT_COPY);
    AURORA_TEST_CHECK_TRUE(multi.outcome == SystemDragOutcome::Succeeded);
    AURORA_TEST_CHECK_TRUE(multi.action == DragAction::Copy);
}

AURORA_TEST_CASE(translate_drag_out_dropped_without_effect_is_rejected) {
    // 落放发生但效果位为零：目标侧不接受该拖放（DoDragDrop 契约的 DROPEFFECT_NONE 语义）。
    const auto rejected = result_from_win32_drag_out(AURORA_EXIT_DROPPED, 0U);
    AURORA_TEST_CHECK_TRUE(rejected.outcome == SystemDragOutcome::Rejected);
    AURORA_TEST_CHECK_TRUE(rejected.action == DragAction::None);
}

AURORA_TEST_CASE(translate_drag_out_cancel_paths_map_to_cancelled) {
    // 取消与「未发生落放」都是用户意图路径，不是故障：一律 Cancelled（不冒充失败）。
    for (const Win32DragOutExit exit : {AURORA_EXIT_CANCELLED, AURORA_EXIT_DEFAULT_CURSORS}) {
        const auto result = result_from_win32_drag_out(exit, 0U);
        AURORA_TEST_CHECK_TRUE(result.outcome == SystemDragOutcome::Cancelled);
        AURORA_TEST_CHECK_TRUE(result.action == DragAction::None);
    }
}

AURORA_TEST_CASE(translate_drag_out_unexpected_exit_is_failed) {
    // 异常 HRESULT（E_UNEXPECTED 一族）→ Failed；效果位不参与（会话没有谈成任何动作）。
    const auto failed = result_from_win32_drag_out(AURORA_EXIT_UNEXPECTED, AURORA_EFFECT_COPY);
    AURORA_TEST_CHECK_TRUE(failed.outcome == SystemDragOutcome::Failed);
    AURORA_TEST_CHECK_TRUE(failed.action == DragAction::None);
}

AURORA_TEST_CASE(translate_wayland_offer_names_map_registered_mime) {
    // text/plain 挂「带字符集 + 裸形」两枚（源侧同时广告：对端文件管理器两种都可能给）。
    const std::vector<std::string> text = wayland_offer_names_from_mime(AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_REQUIRE_EQ(text.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(text[0], "text/plain;charset=utf-8");
    AURORA_TEST_CHECK_STREQ(text[1], "text/plain");

    const std::vector<std::string> uris = wayland_offer_names_from_mime(AURORA_MIME_URI_LIST);
    AURORA_TEST_REQUIRE_EQ(uris.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(uris[0], "text/uri-list");

    const std::vector<std::string> png = wayland_offer_names_from_mime(AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_REQUIRE_EQ(png.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(png[0], "image/png");

    // 未登记 MIME（含进程内的 aurora/widget）不映射：不猜、不兜底成空串。
    AURORA_TEST_CHECK_TRUE(wayland_offer_names_from_mime(AURORA_MIME_TEXT_HTML).empty());
    AURORA_TEST_CHECK_TRUE(wayland_offer_names_from_mime(AURORA_MIME_WIDGET).empty());
    AURORA_TEST_CHECK_TRUE(wayland_offer_names_from_mime("application/x-custom").empty());
    AURORA_TEST_CHECK_TRUE(wayland_offer_names_from_mime("").empty());
}

AURORA_TEST_CASE(translate_wayland_offer_name_reverse_strips_parameters_for_text_plain) {
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("text/plain;charset=utf-8")),
                            AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("text/plain")), AURORA_MIME_TEXT_PLAIN);
    // 对端参数集合不可控：`encoding=` 之类也须归一到同一个规范形（按字面量比会把一个格式判成两个）。
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("text/plain;charset=us-ascii")),
                            AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("text/plain;charset=utf-8;foo=bar")),
                            AURORA_MIME_TEXT_PLAIN);

    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("text/uri-list")), AURORA_MIME_URI_LIST);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_wayland_offer_name("image/png")), AURORA_MIME_IMAGE_PNG);

    // 非 text/plain 的参数化形不放宽：没有互操作先例，放宽只会放大误判面。
    AURORA_TEST_CHECK_FALSE(mime_from_wayland_offer_name("image/png;foo=bar").has_value());
    AURORA_TEST_CHECK_FALSE(mime_from_wayland_offer_name("UTF8_STRING").has_value());
    AURORA_TEST_CHECK_FALSE(mime_from_wayland_offer_name("").has_value());
}

AURORA_TEST_CASE(translate_wayland_mime_list_preserves_priority_and_dedups) {
    const std::vector<std::string> names{
        "image/png",    "text/plain;charset=utf-8", "text/plain", "text/uri-list", "UTF8_STRING", "image/png",
        "aurora/widget"};
    const std::vector<std::string> mimes = mimes_from_wayland_offer_names(names);
    AURORA_TEST_REQUIRE_EQ(mimes.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(mimes[0], AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_CHECK_STREQ(mimes[1], AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(mimes[2], AURORA_MIME_URI_LIST);

    AURORA_TEST_CHECK_TRUE(mimes_from_wayland_offer_names({}).empty());
    AURORA_TEST_CHECK_TRUE(mimes_from_wayland_offer_names({"UTF8_STRING", "aurora/widget"}).empty());
}

AURORA_TEST_CASE(translate_wayland_dnd_action_bits_map_both_ways) {
    AURORA_TEST_CHECK_EQ(wayland_dnd_action_from(DragAction::Copy), static_cast<std::uint32_t>(WaylandDndAction::Copy));
    AURORA_TEST_CHECK_EQ(wayland_dnd_action_from(DragAction::Move), static_cast<std::uint32_t>(WaylandDndAction::Move));
    // None / Link 桥不声明（Link 在核心拖放里几乎不被目标侧声明）。
    AURORA_TEST_CHECK_EQ(wayland_dnd_action_from(DragAction::None), std::uint32_t{0});
    AURORA_TEST_CHECK_EQ(wayland_dnd_action_from(DragAction::Link), std::uint32_t{0});

    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(static_cast<std::uint32_t>(WaylandDndAction::Copy)) ==
                           DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(static_cast<std::uint32_t>(WaylandDndAction::Move)) ==
                           DragAction::Move);
    // 多位按 Copy → Move 优先序取一（与 DROPEFFECT 侧同源：复制最不易造成数据损失）。
    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(static_cast<std::uint32_t>(WaylandDndAction::Copy) |
                                                          static_cast<std::uint32_t>(WaylandDndAction::Move)) ==
                           DragAction::Copy);
    // Ask 位与未知位：本库无「询问用户」态，如实为 None（不替用户猜一个）。
    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(static_cast<std::uint32_t>(WaylandDndAction::Ask)) ==
                           DragAction::None);
    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(std::uint32_t{0xFFU}) == DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_wayland_dnd_action(std::uint32_t{0U}) == DragAction::None);
}

AURORA_TEST_CASE(translate_wayland_registered_lists_are_the_single_source) {
    const std::vector<std::string> &flat = wayland_registered_offer_names();
    AURORA_TEST_REQUIRE_EQ(flat.size(), std::size_t{4});
    AURORA_TEST_CHECK_STREQ(flat[0], "text/plain;charset=utf-8");
    AURORA_TEST_CHECK_STREQ(flat[1], "text/plain");
    AURORA_TEST_CHECK_STREQ(flat[2], "text/uri-list");
    AURORA_TEST_CHECK_STREQ(flat[3], "image/png");
    // 展平表必须与逐项映射同源：改一处不改另一处即双真源（桥按展平表登记、按逐项表取值）。
    for (const std::string &name : flat) {
        AURORA_TEST_REQUIRE_TRUE(mime_from_wayland_offer_name(name).has_value());
    }
    AURORA_TEST_CHECK_EQ(wayland_supported_dnd_actions(), static_cast<std::uint32_t>(WaylandDndAction::Copy) |
                                                              static_cast<std::uint32_t>(WaylandDndAction::Move));
}

AURORA_TEST_CASE(translate_macos_pasteboard_types_map_registered_mime) {
    const std::vector<std::string> text = macos_pasteboard_types_from_mime(AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_REQUIRE_EQ(text.size(), std::size_t{2});
    // 现代 UTI 在前、legacy 常量名在后：顺序即优先级（读取比较按该序取首个可用项）。
    AURORA_TEST_CHECK_STREQ(text[0], "public.utf8-plain-text");
    AURORA_TEST_CHECK_STREQ(text[1], "NSStringPboardType");

    const std::vector<std::string> uris = macos_pasteboard_types_from_mime(AURORA_MIME_URI_LIST);
    AURORA_TEST_REQUIRE_EQ(uris.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(uris[0], "public.file-url");

    const std::vector<std::string> png = macos_pasteboard_types_from_mime(AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_REQUIRE_EQ(png.size(), std::size_t{1});
    AURORA_TEST_CHECK_STREQ(png[0], "public.png");

    // 未登记 MIME：不猜、不兜底（与另三平台表同纪律）。
    AURORA_TEST_CHECK_TRUE(macos_pasteboard_types_from_mime(AURORA_MIME_TEXT_HTML).empty());
    AURORA_TEST_CHECK_TRUE(macos_pasteboard_types_from_mime(AURORA_MIME_WIDGET).empty());
    AURORA_TEST_CHECK_TRUE(macos_pasteboard_types_from_mime("application/x-custom").empty());
    AURORA_TEST_CHECK_TRUE(macos_pasteboard_types_from_mime("").empty());
}

AURORA_TEST_CASE(translate_macos_pasteboard_type_reverse_maps_registered_types) {
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_macos_pasteboard_type("public.utf8-plain-text")),
                            AURORA_MIME_TEXT_PLAIN);
    // legacy 常量名同归 text/plain：载荷同一份 UTF-8 字节，解码不分叉。
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_macos_pasteboard_type("NSStringPboardType")),
                            AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_macos_pasteboard_type("public.file-url")), AURORA_MIME_URI_LIST);
    AURORA_TEST_CHECK_STREQ(require_value(mime_from_macos_pasteboard_type("public.png")), AURORA_MIME_IMAGE_PNG);

    // legacy 文件清单类型不登记：其载荷是路径串数组而非 URI 清单，解码形态不同（不声称能读）。
    AURORA_TEST_CHECK_FALSE(mime_from_macos_pasteboard_type("NSFilenamesPboardType").has_value());
    // TIFF 不登记：本库不解 TIFF（macOS 应用更常写它，如实不可得）。
    AURORA_TEST_CHECK_FALSE(mime_from_macos_pasteboard_type("public.tiff").has_value());
    AURORA_TEST_CHECK_FALSE(mime_from_macos_pasteboard_type("").has_value());
}

AURORA_TEST_CASE(translate_macos_mime_list_preserves_priority_and_dedups) {
    const std::vector<std::string> types{"public.png",      "public.utf8-plain-text", "NSStringPboardType",
                                         "public.file-url", "NSFilenamesPboardType",  "public.png",
                                         "aurora/widget"};
    const std::vector<std::string> mimes = mimes_from_macos_pasteboard_types(types);
    AURORA_TEST_REQUIRE_EQ(mimes.size(), std::size_t{3});
    AURORA_TEST_CHECK_STREQ(mimes[0], AURORA_MIME_IMAGE_PNG);
    AURORA_TEST_CHECK_STREQ(mimes[1], AURORA_MIME_TEXT_PLAIN);
    AURORA_TEST_CHECK_STREQ(mimes[2], AURORA_MIME_URI_LIST);

    AURORA_TEST_CHECK_TRUE(mimes_from_macos_pasteboard_types({}).empty());
    AURORA_TEST_CHECK_TRUE(mimes_from_macos_pasteboard_types({"NSFilenamesPboardType", "aurora/widget"}).empty());
}

AURORA_TEST_CASE(translate_macos_drag_operation_bits_map_both_ways) {
    // ⚠️ 位序与 Win32 DROPEFFECT / Wayland dnd_action 都不同：Link=2 而 Move=16。
    // 把本函数写成复用 Win32 侧同名实现（按 DROPEFFECT 位序）即转红。
    AURORA_TEST_CHECK_EQ(macos_drag_operation_from(DragAction::Copy),
                         static_cast<std::uint32_t>(MacOSDragOperation::Copy));
    AURORA_TEST_CHECK_EQ(macos_drag_operation_from(DragAction::Move),
                         static_cast<std::uint32_t>(MacOSDragOperation::Move));
    AURORA_TEST_CHECK_EQ(macos_drag_operation_from(DragAction::Link),
                         static_cast<std::uint32_t>(MacOSDragOperation::Link));
    AURORA_TEST_CHECK_EQ(macos_drag_operation_from(DragAction::None), std::uint32_t{0});

    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Copy)) ==
                           DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Move)) ==
                           DragAction::Move);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Link)) ==
                           DragAction::Link);
    // 多位按 Copy → Move → Link 优先序取一（与另三平台同源：复制最不易造成数据损失）。
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Copy) |
                                                            static_cast<std::uint32_t>(MacOSDragOperation::Move)) ==
                           DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Move) |
                                                            static_cast<std::uint32_t>(MacOSDragOperation::Link)) ==
                           DragAction::Move);
    // Generic / Private / Delete：语义归目标应用或源侧特殊态，本库无对应态，如实为 None。
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Generic)) ==
                           DragAction::None);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Private)) ==
                           DragAction::None);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Delete)) ==
                           DragAction::None);
    // Every（全位掩码，源侧「任意皆可」）：按优先序落 Copy（最不易损的解读）。
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(static_cast<std::uint32_t>(MacOSDragOperation::Every)) ==
                           DragAction::Copy);
    AURORA_TEST_CHECK_TRUE(action_from_macos_drag_operation(std::uint32_t{0U}) == DragAction::None);
}

AURORA_TEST_CASE(translate_macos_registered_lists_are_the_single_source) {
    const std::vector<std::string> &flat = macos_registered_pasteboard_types();
    AURORA_TEST_REQUIRE_EQ(flat.size(), std::size_t{4});
    AURORA_TEST_CHECK_STREQ(flat[0], "public.utf8-plain-text");
    AURORA_TEST_CHECK_STREQ(flat[1], "NSStringPboardType");
    AURORA_TEST_CHECK_STREQ(flat[2], "public.file-url");
    AURORA_TEST_CHECK_STREQ(flat[3], "public.png");
    // 展平表必须与逐项映射同源：改一处不改另一处即双真源（桥按展平表 registerForDraggedTypes:、
    // 按逐项表取值）。
    for (const std::string &type : flat) {
        AURORA_TEST_REQUIRE_TRUE(mime_from_macos_pasteboard_type(type).has_value());
    }
    AURORA_TEST_CHECK_EQ(macos_supported_drag_operations(), static_cast<std::uint32_t>(MacOSDragOperation::Copy) |
                                                                static_cast<std::uint32_t>(MacOSDragOperation::Move));
}

}  // namespace aurora::test_cases::utest_system_drag_translate
