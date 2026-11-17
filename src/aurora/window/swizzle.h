// swizzle.h —— 内部共享像素 swizzle 辅助（仅库实现可见，不属公共 API）。
#pragma once

// <cstddef> 不可省：本头只依赖 std::size_t，其他平台靠上游头间接带入它，
// 但 wasm 配置下没有那份间接包含，`no type named 'size_t' in namespace 'std'`。
#include <cstddef>
#include <cstdint>

#include "aurora/core/platform.h"

/// @brief x86 架构判定：显式 SIMD 内核仅 x86 可用；架构判定统一走 core/platform.h 的
/// AURORA_ARCH_*，不直接书写原生架构宏（见 06-app-platform.md §12.1）。
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_X86)
#define AURORA_SWIZZLE_SIMD_X86
#endif

/// @brief AVX2 内核可用性：需 x86 + GCC/Clang 的逐函数 target 属性。MSVC 无此属性，
/// 保持编译器自动向量化的基线循环（同一语义，仅档位不同）。
#if defined(AURORA_SWIZZLE_SIMD_X86) && (defined(__GNUC__) || defined(__clang__))
#define AURORA_SWIZZLE_AVX2
#include <immintrin.h>
#endif

#ifdef AURORA_SWIZZLE_AVX2
/// @brief AVX2 内核的函数级目标属性：本头其余代码按基线指令集编译，内核单独升档。
#define AURORA_SWIZZLE_AVX2_TARGET __attribute__((target("avx2")))
#endif

namespace aurora {

// NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)

// ---- 标量黄金参考（与显式 SIMD 内核逐字节一致）----

/// @brief RGBA（Painter 内存序，小端：R,G,B,A）→ BGRA 标量参考：交换 R/B，保留 G/A。
/// 与 Win32 DIB / X11 常见 BGRX 情形等价；紧密位运算，-O3 下自动向量化。
/// 此前 win32_surface 与 wayland_surface 各有一份字节相同的实现，统一于此。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_rgba_to_bgra_scalar(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFU) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

/// @brief BGRA（Win32 DIB / PrintWindow 输出字节序）→ RGBA 标量参考：交换 R/B，保留 G/A。
/// 与 `swizzle_rgba_to_bgra_scalar` 对称：R/B 交换在 32 位打包下是同一位操作，故实现等价。
/// 用于真实窗口截图（`capture_window`）把 GDI BGRA 缓冲转回 Painter RGBA 序后写 PNG。
/// @param src 源像素首指针（每像素 4 字节，小端 B,G,R,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 R,G,B,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_bgra_to_rgba_scalar(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        dst[i] = (px & 0xFF00FF00U) | ((px & 0xFFU) << 16U) | ((px >> 16U) & 0xFFU);
    }
}

/// @brief RGBA 直色（Painter 内存序）→ BGRA **预乘 alpha** 标量参考（Wayland wl_shm
/// `WL_SHM_FORMAT_ARGB8888` 专用）。
/// Wayland 合成器按预乘 alpha 解读 ARGB8888：直色上传会让半透明像素（CSD 阴影带）出现
/// 颜色发灰/发暗的混合错误。输出字布局同 `swizzle_rgba_to_bgra_scalar`（小端内存 B,G,R,A），
/// 仅 R/B 通道在交换前先按 `c * a / 255` 预乘；alpha=255（不透明内容区）预乘为恒等，
/// alpha=0（margin 外缘）RGB 归零——两端均无损。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_rgba_premul_to_bgra_scalar(const std::uint32_t *src, std::uint32_t *dst, std::size_t count)
    -> void {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t px = src[i];
        const std::uint32_t r = px & 0xFFU;
        const std::uint32_t g = (px >> 8U) & 0xFFU;
        const std::uint32_t b = (px >> 16U) & 0xFFU;
        const std::uint32_t a = (px >> 24U) & 0xFFU;
        const std::uint32_t rp = ((r * a) + 0xFFU) >> 8;  // 除以 255 的整数近似（误差 ≤ 1 LSB）
        const std::uint32_t gp = ((g * a) + 0xFFU) >> 8;
        const std::uint32_t bp = ((b * a) + 0xFFU) >> 8;
        dst[i] = (a << 24U) | (rp << 16U) | (gp << 8U) | bp;
    }
}

#ifdef AURORA_SWIZZLE_AVX2
namespace detail {

// - *-pro-type-reinterpret-cast：向量装载/存储的 `_mm256_loadu_si256(reinterpret_cast<__m256i*>(...))`
//   是 SSE/AVX 内置函数的固有签名形态，无第二写法（与 painter_simd.inl 同一豁免口径）。
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

/// @brief 运行时探测 AVX2 可用性（`__builtin_cpu_supports` 内含 OS XSAVE 状态检查），首次调用后缓存。
/// @return CPU 与 OS 均支持 AVX2 时为 true；否则 false（调用方回落标量参考）。
inline auto swizzle_avx2_available() noexcept -> bool {
    static const bool AVX2_AVAILABLE = __builtin_cpu_supports("avx2");
    return AVX2_AVAILABLE;
}

/// @brief 8 像素一组的 R/B 交换内核（`pshufb` 字节置换，等价于标量的位交换），尾部回落标量参考。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数（不足 8 时整段走标量参考）。
AURORA_SWIZZLE_AVX2_TARGET inline auto swap_rb_avx2(const std::uint32_t *src, std::uint32_t *dst, std::size_t count)
    -> void {
    // 每 4 字节组内按 [2,1,0,3] 重排：R,G,B,A → B,G,R,A（两半各一份掩码）。
    const __m256i order = _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15, 2, 1, 0, 3, 6, 5, 4, 7,
                                           10, 9, 8, 11, 14, 13, 12, 15);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i px = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), _mm256_shuffle_epi8(px, order));
    }
    swizzle_rgba_to_bgra_scalar(src + i, dst + i, count - i);
}

/// @brief 8 像素一组的预乘 + R/B 交换内核（32 位整数乘，与标量参考同式同截断），尾部回落标量参考。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数（不足 8 时整段走标量参考）。
AURORA_SWIZZLE_AVX2_TARGET inline auto premul_bgra_avx2(const std::uint32_t *src, std::uint32_t *dst, std::size_t count)
    -> void {
    const __m256i c255 = _mm256_set1_epi32(255);
    const __m256i byte_mask = _mm256_set1_epi32(0xFF);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i px = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + i));
        const __m256i a = _mm256_srli_epi32(px, 24);
        const __m256i r = _mm256_and_si256(px, byte_mask);
        const __m256i g = _mm256_and_si256(_mm256_srli_epi32(px, 8), byte_mask);
        const __m256i b = _mm256_and_si256(_mm256_srli_epi32(px, 16), byte_mask);
        // (c * a + 255) >> 8：乘积 ≤ 65025，32 位乘法不溢出，与标量结果逐位相同。
        const __m256i rp = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(r, a), c255), 8);
        const __m256i gp = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(g, a), c255), 8);
        const __m256i bp = _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(b, a), c255), 8);
        const __m256i out = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(a, 24), _mm256_slli_epi32(rp, 16)),
                                            _mm256_or_si256(_mm256_slli_epi32(gp, 8), bp));
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), out);
    }
    swizzle_rgba_premul_to_bgra_scalar(src + i, dst + i, count - i);
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace detail
#endif

/// @brief RGBA → BGRA 分发入口：AVX2 可用且像素数足够一组时走显式内核，否则走标量参考。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_rgba_to_bgra(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
#ifdef AURORA_SWIZZLE_AVX2
    if (count >= 8 && detail::swizzle_avx2_available()) {
        detail::swap_rb_avx2(src, dst, count);
        return;
    }
#endif
    swizzle_rgba_to_bgra_scalar(src, dst, count);
}

/// @brief BGRA → RGBA 分发入口：与 `swizzle_rgba_to_bgra` 共用内核（同一位交换），逐字节等价。
/// @param src 源像素首指针（每像素 4 字节，小端 B,G,R,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 R,G,B,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_bgra_to_rgba(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
#ifdef AURORA_SWIZZLE_AVX2
    if (count >= 8 && detail::swizzle_avx2_available()) {
        detail::swap_rb_avx2(src, dst, count);
        return;
    }
#endif
    swizzle_bgra_to_rgba_scalar(src, dst, count);
}

/// @brief RGBA 直色 → BGRA 预乘 alpha 分发入口：AVX2 可用且像素数足够一组时走显式内核，否则走标量参考。
/// @param src 源像素首指针（每像素 4 字节，小端 R,G,B,A）。
/// @param dst 目标像素首指针（每像素 4 字节，小端 B,G,R,A）；不可与 `src` 重叠。
/// @param count 像素个数。
inline auto swizzle_rgba_premul_to_bgra(const std::uint32_t *src, std::uint32_t *dst, std::size_t count) -> void {
#ifdef AURORA_SWIZZLE_AVX2
    if (count >= 8 && detail::swizzle_avx2_available()) {
        detail::premul_bgra_avx2(src, dst, count);
        return;
    }
#endif
    swizzle_rgba_premul_to_bgra_scalar(src, dst, count);
}

// NOLINTEND(*-pro-bounds-pointer-arithmetic)

}  // namespace aurora
