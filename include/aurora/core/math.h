#pragma once

/// @brief 渲染值域饱和助手：统一 0/1、0/255 钳制端点，内部收敛使用（不进 `aurora.h` 公共导出）。
/// @file
#include <algorithm>
#include <cstdint>

namespace aurora {

/// @brief 饱和到 [0, 1]：负数夹到 0、大于 1 夹到 1。
///
/// 收口散落的 `std::clamp`，保证渲染像素/覆盖度路径端点恒定；语义与 `std::clamp(x, 0, 1)` 逐位等价。
/// @param x 待钳制的浮点值。
/// @return 钳制到 [0, 1] 的浮点值。
constexpr auto saturate(float x) noexcept -> float { return std::clamp(x, 0.0F, 1.0F); }

/// @brief 饱和到 8 位无符号：先 `std::clamp(x, 0, 255)` 再转 `std::uint8_t`。
/// @param x 待钳制的浮点值。
/// @return 越界夹到 0 或 255 的字节值。
constexpr auto saturate_u8(float x) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>(std::clamp(x, 0.0F, 255.0F));
}

}  // namespace aurora
