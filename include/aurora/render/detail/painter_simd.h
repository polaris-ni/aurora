// Aurora — 光栅内核 SIMD 双实现
// 约定：
//  - 标量参考实现（*_scalar）与现有渲染像素逐位一致，golden 以此为准。
//  - SIMD 路径（*_sse2 / *_avx2）镜像标量浮点运算序列；编译期加 -ffp-contract=off
//    杜绝 FMA 融合，x86-64 下 packed float 与标量 float 逐位相同。
//  - 整型转换用 cvtt（向零截断），与 static_cast<int>(float) 语义一致。
//  - 默认 g_simd_level = SSE2（x86-64 基线恒可用）；AVX2 走运行时 CPUID 分发；
//    ARM/NEON 本轮暂缓，回落 Scalar。
#pragma once
#include <algorithm>
#include <cstdint>

#include "aurora/render/detail/gamma_lut.h"

namespace aurora::detail {

enum class SimdLevel : std::uint8_t { Scalar, SSE2, AVX2 };

// x86 架构判定：SSE2/AVX2 内置函数与 target 属性仅 x86 可用；非 x86（ARM/NEON，本轮暂缓）
// 回落 Scalar，分发只走标量黄金路径。定义 AURORA_SIMD_X86 供 .inl / 测试统一判定。
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define AURORA_SIMD_X86 1  // NOLINT(*-macro-usage)
#endif

// 运行时探测一次；x86-64 至少 SSE2，AVX2 按需。
auto detect_simd_level() noexcept -> SimdLevel;

// 懒初始化（首次混合前调用一次）。
auto ensure_simd_init() noexcept -> void;

// 进程级 SIMD 能力级别（懒探测一次，见 ensure_simd_init()）。
// inline 变量替代「extern 声明 + 外部定义」两段式：链接器保证跨 TU 单一对象，
// 且 dispatch 热路径 switch(g_simd_level) 可直读，零间接开销；刻意不包访问器，
// 避免热路径函数调用。非 SIMD 构建下该变量无引用者（仅一字节枚举，无副作用）。
inline SimdLevel g_simd_level = SimdLevel::SSE2;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): x86-64
                                                  // 基线；运行时 detect 后可能升为 AVX2

// ---- 标量黄金参考（与现有像素逐位一致）----
// 伽马混合：逐通道 alpha（ar/ag/ab），覆盖文本 AA 的 per-channel 覆盖率。
auto blend_srgb_over_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                   float ag, float ab, int n) -> void;
auto blend_linear_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                float finv, int n) -> void;

// 整数 box blur（分离式两遍）标量黄金参考；与 Painter::blur_region 逐位一致。
// pixels：帧缓冲（步长 full_width*4）；区域 (x0,y0,rw,rh)；窗口半径 r（n = 2r+1 恒定）。
auto blur_region_scalar(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;

// 渐变扫描线标量黄金参考（单行，真·逐像素公式，与 SIMD 逐位一致）。
// row 指向该行 x0 处的首字节；填充 [x0, x0+n) 共 n 个像素；不透明双色标（a 恒为 255）。
// 浮点运算序列刻意与 SIMD 版本逐位一致（-ffp-contract=off，无 FMA；sqrt/min/max/div/截断 1:1 映射）。
auto gradient_linear_scanline_scalar(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                     float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                     float range) -> int;
auto gradient_radial_scanline_scalar(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                     const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;

// ---- 显式 SIMD 实现（供 test_simd_parity 直接比对）----
#ifdef AURORA_ENABLE_SIMD
auto blend_srgb_over_region_sse2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                 float ag, float ab, int n) -> void;
auto blend_linear_region_sse2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa, float finv,
                              int n) -> void;
auto blend_srgb_over_region_avx2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                 float ag, float ab, int n) -> void;
auto blend_linear_region_avx2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa, float finv,
                              int n) -> void;
auto blur_region_sse2(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;
auto blur_region_avx2(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;
auto gradient_linear_scanline_sse2(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                   float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                   float range) -> int;
auto gradient_linear_scanline_avx2(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                   float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                   float range) -> int;
auto gradient_radial_scanline_sse2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                   const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;
auto gradient_radial_scanline_avx2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                   const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;
#endif

// ---- 分发入口（生产路径调用）----
#ifdef AURORA_ENABLE_SIMD
inline auto blend_srgb_over_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                   float ag, float ab, int n) -> void {
    init_gamma_tables();
    ensure_simd_init();
#ifdef AURORA_SIMD_X86
    switch (g_simd_level) {
        case SimdLevel::AVX2:
            blend_srgb_over_region_avx2(px, sr, sg, sb, ar, ag, ab, n);
            return;
        case SimdLevel::SSE2:
            blend_srgb_over_region_sse2(px, sr, sg, sb, ar, ag, ab, n);
            return;
        default:
            break;
    }
#endif
    blend_srgb_over_region_scalar(px, sr, sg, sb, ar, ag, ab, n);
}
inline auto blend_linear_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                float finv, int n) -> void {
    ensure_simd_init();
#ifdef AURORA_SIMD_X86
    switch (g_simd_level) {
        case SimdLevel::AVX2:
            blend_linear_region_avx2(px, sr, sg, sb, fa, finv, n);
            return;
        case SimdLevel::SSE2:
            blend_linear_region_sse2(px, sr, sg, sb, fa, finv, n);
            return;
        default:
            break;
    }
#endif
    blend_linear_region_scalar(px, sr, sg, sb, fa, finv, n);
}

// 整数 box blur 分发（生产路径调用）。SIMD 与标量逐位一致。
inline auto blur_region(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void {
    ensure_simd_init();
#ifdef AURORA_SIMD_X86
    switch (g_simd_level) {
        case SimdLevel::AVX2:
            blur_region_avx2(pixels, full_width, x0, y0, rw, rh, r);
            return;
        case SimdLevel::SSE2:
            blur_region_sse2(pixels, full_width, x0, y0, rw, rh, r);
            return;
        default:
            break;
    }
#endif
    blur_region_scalar(pixels, full_width, x0, y0, rw, rh, r);
}

// 渐变扫描线分发（生产路径调用）。SIMD 填充向量宽度整数倍像素，尾部由标量黄金补齐；
// SIMD 与标量逐位一致，故整行结果等同标量黄金。
inline auto gradient_linear_fill(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                 float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                 float range) -> void {
    ensure_simd_init();
#ifdef AURORA_SIMD_X86
    switch (g_simd_level) {
        case SimdLevel::AVX2: {
            const int d = gradient_linear_scanline_avx2(row, x0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0, range);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            gradient_linear_scanline_scalar(row + (static_cast<std::size_t>(d) * 4U), x0 + d, n - d, sx, py, dx, dy,
                                            inv_len_sq, c0, c1, stop0, range);
            return;
        }
        case SimdLevel::SSE2: {
            const int d = gradient_linear_scanline_sse2(row, x0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0, range);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            gradient_linear_scanline_scalar(row + (static_cast<std::size_t>(d) * 4U), x0 + d, n - d, sx, py, dx, dy,
                                            inv_len_sq, c0, c1, stop0, range);
            return;
        }
        default:
            break;
    }
#endif
    gradient_linear_scanline_scalar(row, x0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0, range);
}
inline auto gradient_radial_fill(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                 const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> void {
    ensure_simd_init();
#ifdef AURORA_SIMD_X86
    switch (g_simd_level) {
        case SimdLevel::AVX2: {
            const int d = gradient_radial_scanline_avx2(row, x0, n, cx, py, inv_r, c0, c1, stop0, range);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            gradient_radial_scanline_scalar(row + (static_cast<std::size_t>(d) * 4U), x0 + d, n - d, cx, py, inv_r, c0,
                                            c1, stop0, range);
            return;
        }
        case SimdLevel::SSE2: {
            const int d = gradient_radial_scanline_sse2(row, x0, n, cx, py, inv_r, c0, c1, stop0, range);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            gradient_radial_scanline_scalar(row + (static_cast<std::size_t>(d) * 4U), x0 + d, n - d, cx, py, inv_r, c0,
                                            c1, stop0, range);
            return;
        }
        default:
            break;
    }
#endif
    gradient_radial_scanline_scalar(row, x0, n, cx, py, inv_r, c0, c1, stop0, range);
}
#else
inline auto blend_srgb_over_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                   float ag, float ab, int n) -> void {
    blend_srgb_over_region_scalar(px, sr, sg, sb, ar, ag, ab, n);
}
inline auto blend_linear_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                float finv, int n) -> void {
    blend_linear_region_scalar(px, sr, sg, sb, fa, finv, n);
}

inline auto blur_region(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void {
    blur_region_scalar(pixels, full_width, x0, y0, rw, rh, r);
}

inline auto gradient_linear_fill(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                 float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                 float range) -> void {
    gradient_linear_scanline_scalar(row, x0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0, range);
}
inline auto gradient_radial_fill(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                 const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> void {
    gradient_radial_scanline_scalar(row, x0, n, cx, py, inv_r, c0, c1, stop0, range);
}
#endif

}  // namespace aurora::detail