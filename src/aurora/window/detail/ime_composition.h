#pragma once

// IME 组合串索引折算：**内部头**（与 `win32_ua.h` / `win32_cursor.h` 同列于 src/）。
//
// 为何单列且零平台依赖：各平台输入法 API 对「组合串 + 光标 + 待转换选区」的索引口径
// **都不相同**——Win32 IMM32 给 UTF-16 串与 **WCHAR 下标**（`GCS_CURSORPOS` / `GCS_COMPATTR`
// 逐单元属性），macOS `NSTextInputClient` 给 marked range（UTF-16），Wayland/IBus 给 UTF-8
// 与 `index_in_text`（UTF-8 字节）。而 Aurora 的 `TextCompositionEvent` 契约是
// **UTF-8 串 + 码点下标**（见 `event/event.h`）。故「平台索引 → 码点下标」这一层
// 折算必须存在，且必须是**可无头单测**的纯函数：真机输入法不可自动化，索引错位这种
// 逐字符级缺陷只有纯函数测试才兜得住（emoji / 代理对 / 孤立代理是最易错的四类输入）。
//
// 本头不引任何 `<windows.h>` / Cocoa / Xlib / wayland 头：UTF-16 路入参用 `char16_t` 与
// `std::uint8_t`（平台桥把原生 `WCHAR`/`unichar` 数组原样转传），UTF-8 路入参用 `std::string_view`
// 与 `std::int64_t` 字节下标（X11 XIM 组合串、Wayland text-input-v3 `index_in_text` 原样转传）。

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "aurora/event/event.h"

namespace aurora::ime {

/// @brief IMM32 `GCS_COMPATTR` 的取值（MSDN「GCS_COMPATTR」原文数值，逐 UTF-16 单元一字节）。
///
/// 本地声明而非取 SDK 宏：MinGW 的 `<imm.h>` 对这批常量覆盖不全，且各平台 SDK 命名不一，
/// 折算层只依赖**数值语义**。`kTargetConverted` / `kTargetNotConverted` 两段即
/// 「输入法当前高亮待选的转换段」，映射到 `TextCompositionEvent::sel_start` / `sel_end`。
enum Attr : std::uint8_t {
    kTargetConverted = 1,     ///< 已转换且仍是目标段（候选替换区）
    kConverted = 2,           ///< 已转换、非目标段
    kTargetNotConverted = 3,  ///< 未转换且是目标段（待按拼音转换区）
    kInputError = 4,          ///< 输入错误段（下划线呈波浪）
    kFixedConverted = 5,      ///< 已锁定、不再参与转换
};

/// @brief 码点区间（含头含尾）；`end` 为哨兵即「无区间」。
struct CpRange {
    std::size_t start = 0;  ///< 起点码点下标
    /// @brief 终点码点下标（含尾）；无区间时为 `TextCompositionEvent::AURORA_NO_SELECTION`。
    std::size_t end = TextCompositionEvent::AURORA_NO_SELECTION;

    /// @brief 是否存在有效区间。
    [[nodiscard]] auto has_selection() const -> bool { return end != TextCompositionEvent::AURORA_NO_SELECTION; }
};

/// @brief UTF-16 单元下标 → **码点**下标。
///
/// 代理对（非 BMP）占 2 个单元只计 1 个码点；`utf16_index` 落在代理对**中间**（高代理之后）时
/// 向下夹紧到该码点起点，绝不返回指向码点中部的下标（与 `a11y::UtfOffsetMap::to_utf8` 同纪律）。
/// 越界夹紧到总码点数。
[[nodiscard]] auto utf16_index_to_cp_index(std::u16string_view text, std::size_t utf16_index) -> std::size_t;

/// @brief 由 `GCS_COMPATTR` 数组（逐 UTF-16 单元）取「目标转换段」的码点区间。
///
/// 目标段 = 连续的 `kTargetConverted` / `kTargetNotConverted` 单元。IMM32 实际只给单段目标；
/// 出现多段（异常输入）时取**起始最靠前的一段**，保证可稳定复现。
/// @param attrs 逐单元属性；长度与 `text` 单元数不一致时按较短者生效（多余忽略、不足视同缺失）。
/// @return 无属性数组 / 数组全零长 / 无目标段时，`end` 为哨兵（无选区）。
[[nodiscard]] auto target_selection(std::u16string_view text, const std::vector<std::uint8_t> &attrs) -> CpRange;

/// @brief 把平台侧一次 `WM_IME_COMPOSITION` 的三件套折算为 `TextCompositionEvent` 的组合态字段。
///
/// 只做「UTF-16 → UTF-8 + 索引折算 + 选区提取」，不填派发期字段；`committed` 由调用方另行填
/// （IMM32 的 `GCS_RESULTSTR` 与 `GCS_COMPSTR` 可在同一消息内同时到达：落字 + 新组合开始）。
/// @param comp  平台组合串（UTF-16）；空串即「组合结束 / 取消」语义（preedit 空、无选区）。
/// @param caret 组合内光标 **UTF-16 单元**下标（`GCS_CURSORPOS`）；越界自动夹紧。
/// @param attrs 逐单元属性数组（`GCS_COMPATTR`），可为空 = 输入法未提供待转换选区。
/// @return 填好 `preedit` / `cursor_index` / `sel_start` / `sel_end` 的事件（`committed` 留空）。
[[nodiscard]] auto make_preedit_state(std::u16string_view comp,
                                      std::size_t caret,
                                      const std::vector<std::uint8_t> &attrs) -> TextCompositionEvent;

/// @brief UTF-8 字节下标 → **码点**下标。
///
/// Wayland text-input-v3 的 `preedit_string` 游标 `index`、X11 XIM 的组合内光标位都以 **UTF-8 字节
/// 偏移**给出（`index_in_text`），而 `TextCompositionEvent` 契约是码点下标，故须这一层折算。
/// `byte_index` 落在某个多字节码点**中间**（非法切分）时向下夹紧到该码点起点，绝不返回指向码点
/// 中部的下标（与 `utf16_index_to_cp_index`、`a11y::UtfOffsetMap` 同纪律）；越界（≥串尾）夹紧到总码点数。
[[nodiscard]] auto utf8_byte_to_cp_index(std::string_view text, std::size_t byte_index) -> std::size_t;

/// @brief 把 UTF-8 口径的一次组合状态折算为 `TextCompositionEvent` 的组合态字段。
///
/// 与 `make_preedit_state`（UTF-16 口径，供 Win32/macOS）对偶，供 **X11 XIM** 与 **Wayland
/// text-input-v3** 使用——两者都直接把 preedit 作为 UTF-8 串、把游标/选区端点作为 UTF-8 字节偏移
/// 交给客户端。本函数只做「字节下标 → 码点下标 + 越界夹紧」，`committed` 仍由调用方填。
/// @param preedit        平台组合串（UTF-8）；空串 = 组合结束 / 取消（preedit 空、无选区）。
/// @param cursor_byte    组合内光标 UTF-8 字节下标；传 `-1` = 输入法未给游标，退化为串尾。
/// @param sel_begin_byte 待转换选区起点字节下标；传 `-1`（连同 `sel_end_byte`）= 无选区。
/// @param sel_end_byte   待转换选区终点字节下标（**不含**尾，半开区间，贴合 IBus/Fcitx 的
///                       `index` 语义）；折算后落到 `TextCompositionEvent` 的含尾码点区间。
/// @return 填好 `preedit` / `cursor_index` / `sel_start` / `sel_end` 的事件（`committed` 留空）。
[[nodiscard]] auto make_preedit_state_utf8(std::string_view preedit,
                                           std::int64_t cursor_byte,
                                           std::int64_t sel_begin_byte,
                                           std::int64_t sel_end_byte) -> TextCompositionEvent;

}  // namespace aurora::ime
