// Aurora — 内部双向文本（bidi）辅助：段落级 run 视觉重排与基准方向推断。
//
// 设计边界（A2 混排 UBA 切片 → 完整逐字符 UBA）：
// - HarfBuzz 在「单个 run」内已按 Unicode Bidi 算法把嵌的异向子段保持可读（视觉序输出，
//   x_advance 恒正）；故段内 bidi 由 hb 负责，本文件不重做。
// - 真正缺的是「跨 run」：shape_line 按字体面切 run 后，只按逻辑序拼接。本文件提供：
//   ① `bidi_visual_run_order`（UBA-lite：RTL 段落整体右→左翻转 run 顺序）；
//   ② 完整逐字符 UBA（UAX #9）：`uba_levels`（X/W/N/I 规则 → 逐码点嵌入层级）+
//     `uba_visual_order`（L2 层叠反转），供 shape_line 以「面 + 层级」双键切 run 后
//     精确重排（LTR 段内 RTL 子段、RTL 段内数字/拉丁内序保持等）。
// - 显式控制符（LRE/RLE/LRO/RLO/PDF/LRI/RLI/FSI/PDI）参与解析；N0（成对括号，
//   BD16 + N0a/N0c1/N0c2）与 W1（NSM）已实现。
//
// 全部为纯函数，不依赖 FreeType/HarfBuzz，可独立单测。
#pragma once

#include <aurora/core/enums.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace aurora::render::detail {

/// @brief 由整行文本推断段落基准方向（Unicode Bidi P2/P3：首个强方向字符决定；
///        数字/中性字符跳过；全无强方向字符时回退 LTR）。
/// @note 这是简化分类器，覆盖阿拉伯/希伯来/拉丁/西里尔/希腊与数字；足以定段落基准方向。
[[nodiscard]] auto guess_paragraph_direction(std::string_view text) -> TextDirection;

/// @brief 段落级 run 视觉重排（UBA-lite）：给定 run 数量与段落基准方向，返回各 run
///        在视觉序中的下标序列。RTL 段落整体右→左翻转 run 顺序；LTR 段落保持逻辑序。
/// @param run_count 该行的 face-run 数量（必须 > 0，否则返回空）。
/// @param base 段落基准方向（来自 `opts.direction` 或 `guess_paragraph_direction`）。
[[nodiscard]] auto bidi_visual_run_order(std::size_t run_count, TextDirection base) -> std::vector<std::size_t>;

/// @brief 逐码点 UBA 嵌入层级（UAX #9：X1-X9 显式嵌入/隔离 + W1-W7 弱类型 +
///        N0 成对括号 + N1-N2 中性 + I1-I2 隐式层级）。
/// @param text 码点序列（调用方已完成 UTF-8 解码；`'\n'` 等按中性处理）。
/// @param base_level 段落基准层级（0 = LTR 基准，1 = RTL 基准）。
/// @return 与 `text` 等长的层级数组；显式控制符占位为其所在 embedding 的层级
///         （调用方的重排/绘制按需跳过；Aurora 文本流通常不含控制符）。
[[nodiscard]] auto uba_levels(const std::vector<char32_t> &text, std::uint8_t base_level)
    -> std::vector<std::uint8_t>;

/// @brief UBA L2 重排：给定每元素（如 run）的嵌入层级序列，返回视觉序（左→右）的
///        元素下标序列。同层级连续元素构成隐式 span；层叠反转自然保持 span 内部的
///        逻辑序（span 内部字形序由 hb 按 run 级方向负责）。LTR 全零层为恒等变换。
/// @param levels 每元素的 UBA 嵌入层级（奇 = 该元素内容按 RTL 显示）。
[[nodiscard]] auto uba_visual_order(const std::vector<std::uint8_t> &levels) -> std::vector<std::size_t>;

}  // namespace aurora::render::detail
