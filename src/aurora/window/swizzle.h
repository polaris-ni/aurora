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
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFU) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

/// @brief BGRA（Win32 DIB / PrintWindow 输出字节序）→ RGBA 逐行 swizzle（交换 R/B，保留 G/A）。
/// 与 `swizzle_rgba_to_bgra` 对称：R/B 交换在 32 位打包下是同一位操作，故实现等价。
/// 用于真实窗口截图（`capture_window`）把 GDI BGRA 缓冲转回 Painter RGBA 序后写 PNG。
inline auto swizzle_bgra_to_rgba(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFU) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

/// @brief RGBA 直色（Painter 内存序）→ BGRA **预乘 alpha** 逐像素转换（Wayland wl_shm
/// `WL_SHM_FORMAT_ARGB8888` 专用）。
/// Wayland 合成器按预乘 alpha 解读 ARGB8888：直色上传会让半透明像素（CSD 阴影带）出现
/// 颜色发灰/发暗的混合错误。输出字布局同 `swizzle_rgba_to_bgra`（小端内存 B,G,R,A），
/// 仅 R/B 通道在交换前先按 `c * a / 255` 预乘；alpha=255（不透明内容区）预乘为恒等，
/// alpha=0（margin 外缘）RGB 归零——两端均无损。
inline auto swizzle_rgba_premul_to_bgra(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        const std::uint32_t r = px & 0xFFU;
        const std::uint32_t g = (px >> 8U) & 0xFFU;
        const std::uint32_t b = (px >> 16U) & 0xFFU;
        const std::uint32_t a = (px >> 24U) & 0xFFU;
        const std::uint32_t rp = (r * a + 0xFFU) >> 8;  // 除以 255 的整数近似（误差 ≤ 1 LSB）
        const std::uint32_t gp = (g * a + 0xFFU) >> 8;
        const std::uint32_t bp = (b * a + 0xFFU) >> 8;
        dst[i] = (a << 24U) | (rp << 16U) | (gp << 8U) | bp;
    }
}

// NOLINTEND(*-pro-bounds-pointer-arithmetic)

}  // namespace aurora
