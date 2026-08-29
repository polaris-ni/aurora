// swizzle.h —— 内部共享像素 swizzle 辅助（仅库实现可见，不属公共 API）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace aurora {

// NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)

/// @brief RGBA（Painter 内存序，小端：R,G,B,A）→ BGRA 逐行 swizzle（交换 R/B，保留 G/A）。
/// 与 Win32 DIB / X11 常见 BGRX 情形等价；紧密位运算，-O3 下自动向量化。
/// 此前 win32_surface 与 wayland_surface 各有一份字节相同的实现，统一于此。
inline auto swizzle_rgba_to_bgra(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFu) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

/// @brief BGRA（Win32 DIB / PrintWindow 输出字节序）→ RGBA 逐行 swizzle（交换 R/B，保留 G/A）。
/// 与 `swizzle_rgba_to_bgra` 对称：R/B 交换在 32 位打包下是同一位操作，故实现等价。
/// 用于真实窗口截图（`capture_window`）把 GDI BGRA 缓冲转回 Painter RGBA 序后写 PNG。
inline auto swizzle_bgra_to_rgba(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFu) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

// NOLINTEND(*-pro-bounds-pointer-arithmetic)

} // namespace aurora
