// Aurora — 内部双向文本（bidi）辅助：段落级 run 视觉重排与基准方向推断。
//
// 设计边界（A2 混排 UBA 切片，UBA-lite）：
// - HarfBuzz 在「单个 run」内已按 Unicode Bidi 算法把嵌的异向子段保持可读（视觉序输出，
//   x_advance 恒正）；故段内 bidi 由 hb 负责，本文件不重做。
// - 真正缺的是「跨 run」：shape_line 按字体面切 run 后，永远按逻辑序拼接，从不按段落方向
//   做视觉重排。本文件提供段落级 run 重排（RTL 段落整体右→左翻转 run 顺序），补足这一层。
// - 完整逐字符 UBA（弱类型/嵌入层级/N0-N2 规则）为后续工作，本切片不做。
//
// 全部为纯函数，不依赖 FreeType/HarfBuzz，可独立单测。
#pragma once

#include <aurora/core/enums.h>

#include <cstddef>
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

}  // namespace aurora::render::detail
