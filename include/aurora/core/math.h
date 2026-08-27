#pragma once

#include <algorithm>
#include <cstdint>

namespace aurora {

// 饱和（saturate）助手：将值钳制到渲染常用的固定值域，避免手写 `std::clamp`
// 散落各处而导致边界（0/1、0/255）不一致。语义与手写 `std::clamp(..., lo, hi)`
// 逐位等价，仅收口到命名助手以明确「饱和」意图并保证端点恒定。
//
// 仅作内部收敛使用（不进入 `aurora.h` 公共导出）。
//
// 边界与零值约定（重构 painter 像素/覆盖度路径时须保留）：
//   - `saturate`   : 越界一律夹到 [0, 1] 端点，负数→0、大于 1→1。
//   - `saturate_u8`: 先 `std::clamp(x, 0, 255)` 再 `static_cast<std::uint8_t>`，
//                    越界夹到 0/255，与原 `static_cast<std::uint8_t>(std::clamp(v, 0, 255))` 完全等价。

constexpr auto saturate(float x) noexcept -> float {
    return std::clamp(x, 0.0f, 1.0f);
}

constexpr auto saturate_u8(float x) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(std::clamp(x, 0.0f, 255.0f));
}

} // namespace aurora
