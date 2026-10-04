#pragma once

/// @brief Unicode 码点在等宽网格里占几格的判定原语：零宽优先，其余按 East Asian Width。
/// @file
#include <cstdint>

namespace aurora {

/// @brief East Asian Width 中 Ambiguous 类所采用的宽度口径。
///
/// Ambiguous 类（箱线字符、`±`/`°`/`→` 一类符号、拉丁重音字母、全角标点等）存在两种既成
/// 事实：UTF-8 本机与 SSH 场景按单宽呈现，GB18030/GBK 串口与部分日文环境按双宽更正确。
/// 取值对错随场景而变，故本框架不替调用方选边，口径一律以入参传入。
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: pure
enum class AmbiguousWidthMode : std::uint8_t {
    Narrow,  ///< Ambiguous 类占 1 格（UTF-8 本机与 SSH 场景的通行口径）
    Wide,  ///< Ambiguous 类占 2 格（GB18030/GBK 串口一类场景所需）
};

/// @brief 判定 Unicode 码点在等宽网格里占几格。
///
/// 判定次序即这条原语的全部内容：**先**按 General_Category 判零宽（`Mn`/`Me`/`Cf`），
/// **再**按 East Asian Width 判 1/2。次序不可交换——`0300..036F`、`FE00..FE0F` 一类组合
/// 符号在 UCD 里同时带 Ambiguous 标记，先查宽度表就会让 `AmbiguousWidthMode::Wide` 口径下
/// 的一个重音符占 2 格，把它并入的基础格挤成半格错位。
///
/// 数据源为 Unicode CDATA 的 East Asian Width 与 General_Category，版本与许可见
/// `THIRD_PARTY_LICENSES.md`。本函数只回答「占几格」：字素簇的切分、变体选择符对字形的
/// 影响都不在其职责内。
/// @param code_point 待判定的 Unicode 码点。
/// @param ambiguous_mode Ambiguous 类在本场景采用的口径。
/// @return 占位格数：0（不独立占格，须并入前一个基础格）、1 或 2。
/// @note Thread: thread-safe (pure function)
/// @note Side-effects: pure
[[nodiscard]] auto unicode_cell_width(char32_t code_point, AmbiguousWidthMode ambiguous_mode) noexcept -> std::uint8_t;

}  // namespace aurora
