#pragma once

// core/literals.h
//
// 用户定义字面量（需求 #4）统一收敛入口。
//
// 字面量实际声明于各自领域头文件内、归属于 `aurora::literals` 命名空间：
//   - core/dimension.h  ：`_dp` / `_px`  （长度）
//   - core/color.h      ：`_rgb` / `_rgba`（颜色，#RRGGBB / #RRGGBBAA）
//   - core/duration.h   ：`_ms` / `_s`   （时长）
//
// 约定（与编码标准一致）：禁止在头文件中全局 `using namespace au::literals`；
// 仅在使用方的翻译单元（.cpp）内按需 `using namespace au::literals;`。
//
// 示例：
//   #include "aurora/aurora.h"
//   using namespace au::literals;
//   auto width = 100_dp;
//   auto bg    = 0xFF0000_rgb;
//   auto anim  = 250_ms;

#include "aurora/core/color.h"
#include "aurora/core/dimension.h"
#include "aurora/core/duration.h"

namespace aurora {
// 便捷别名：在 TU 内 `using namespace au::literals;` 即可使用全部 UDL。
// 此处仅作文档锚点，无额外符号。
} // namespace aurora
