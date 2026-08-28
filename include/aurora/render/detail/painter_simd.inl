// Aurora — 光栅内核 SIMD 双实现（WS-4【D1】实现）
// 本文件被 painter.cpp 与 tests/test_simd_parity.cpp 共同 include，故全部为 header 内联。
// 浮点运算序列严格镜像标量黄金实现（-ffp-contract=off 由 CMake 保证），整型截断用 cvtt。
#pragma once
#include <cmath>

#include "aurora/render/detail/painter_simd.h"

#ifdef _MSC_VER
#define AURORA_AVX2_TARGET
#define AURORA_SSE41_TARGET
#define AURORA_NOINLINE __declspec(noinline)
#else
#define AURORA_AVX2_TARGET __attribute__((target("avx2")))
// SSE2 路径用到 SSE4.1 内置（cvtepu8_epi32 / min_epi32 / max_epi32），须显式开 target。
#define AURORA_SSE41_TARGET __attribute__((target("sse4.1")))
#define AURORA_NOINLINE __attribute__((noinline))
#endif

// x86 才存在 SSE2/AVX2 内置函数头；非 x86（ARM/NEON，本轮暂缓）不引用，回落标量。
#ifdef AURORA_SIMD_X86
#include <immintrin.h>
#endif

namespace aurora::detail {

// 【性能豁免说明】本文件整体抑制以下检查，仅限此 SIMD 光栅内核：
// - pro-bounds-pointer-arithmetic：SSE/AVX 按 4 字节像素步进是技术本质（px + i*4），
//   区域边界已由调用方的裁剪交集保证（shrink_to_clips 与 set_pixel 逐字一致）；
// - narrowing-conversions（含 bugprone-*）：浮点运算序列必须与标量黄金参考逐位一致
//   （golden 测试逐位比对），添加显式转换会改变取整/运算顺序，属语义风险而非风格问题；
// - readability-math-missing-parentheses：括号不改写运算结果，但会破坏与标量参考
//   的逐位镜像公式排版对照；
// - avoid-c-arrays / pro-type-reinterpret-cast / pro-bounds-constant-array-index /
//   avoid-unchecked-container-access：`alignas(32) int iarr[8]` 对齐暂存、
//   `_mm256_storeu_si256(reinterpret_cast<__m256i*>(px))` 向量装载、C 数组变址
//   均为 SIMD 惯用法；
// - isolate-declaration / avoid-nested-conditional-operator / use-auto：同一行声明
//   8 个通道变量、按通道三元链在 SIMD 代码里成对出现，拆开反而破坏镜像对照。
// 逐像素越界访问本身已收敛到 gamma_lut.h 的唯一可信点（srgb_to_linear / linear_to_srgb_lut）。
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-narrowing-conversions,
// bugprone-narrowing-conversions, readability-math-missing-parentheses, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-pro-type-reinterpret-cast,
// cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-isolate-declaration, readability-avoid-nested-conditional-operator, modernize-use-auto)

// ---------------- 标量黄金参考 ----------------
inline auto blend_srgb_over_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                          float ag, float ab, int n) -> void {
    for (int i = 0; i < n; ++i) {
        std::uint8_t *p = px + static_cast<std::size_t>(i) * 4u;
        p[0] = blend_srgb_over(p[0], sr, ar);
        p[1] = blend_srgb_over(p[1], sg, ag);
        p[2] = blend_srgb_over(p[2], sb, ab);
        p[3] = 255;
    }
}

inline auto blend_linear_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                       float finv, int n) -> void {
    for (int i = 0; i < n; ++i) {
        std::uint8_t *p = px + static_cast<std::size_t>(i) * 4u;
        p[0] = static_cast<std::uint8_t>(p[0] * finv + sr * fa);
        p[1] = static_cast<std::uint8_t>(p[1] * finv + sg * fa);
        p[2] = static_cast<std::uint8_t>(p[2] * finv + sb * fa);
        p[3] = 255;
    }
}

// ---------------- 整数 box blur 标量黄金参考（WS-4：blur_region 栅格原语）----------------
// 与 Painter::blur_region 逐位一致（实为同一算法：分离式两遍整数 box blur，n=2r+1 恒定，
// 结果 = acc[c]/n 正整数截断）。SIMD 路径须与此逐位一致。
inline auto blur_region_scalar(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void {
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(rw) * rh * 4);
    const int n = 2 * r + 1;
    // 第一遍：水平模糊（帧缓冲 → tmp）
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            int acc[4] = { 0, 0, 0, 0 };
            for (int k = -r; k <= r; ++k) {
                const int sx = std::clamp(x + k, 0, rw - 1);
                const std::size_t src = (static_cast<std::size_t>(y0 + y) * full_width + (x0 + sx)) * 4;
                acc[0] += pixels[src];
                acc[1] += pixels[src + 1];
                acc[2] += pixels[src + 2];
                acc[3] += pixels[src + 3];
            }
            const std::size_t dst = (static_cast<std::size_t>(y) * rw + x) * 4;
            tmp[dst] = static_cast<std::uint8_t>(acc[0] / n);
            tmp[dst + 1] = static_cast<std::uint8_t>(acc[1] / n);
            tmp[dst + 2] = static_cast<std::uint8_t>(acc[2] / n);
            tmp[dst + 3] = static_cast<std::uint8_t>(acc[3] / n);
        }
    }
    // 第二遍：垂直模糊（tmp → 帧缓冲）
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            int acc[4] = { 0, 0, 0, 0 };
            for (int k = -r; k <= r; ++k) {
                const int sy = std::clamp(y + k, 0, rh - 1);
                const std::size_t src = (static_cast<std::size_t>(sy) * rw + x) * 4;
                acc[0] += tmp[src];
                acc[1] += tmp[src + 1];
                acc[2] += tmp[src + 2];
                acc[3] += tmp[src + 3];
            }
            const std::size_t dst = (static_cast<std::size_t>(y0 + y) * full_width + (x0 + x)) * 4;
            pixels[dst] = static_cast<std::uint8_t>(acc[0] / n);
            pixels[dst + 1] = static_cast<std::uint8_t>(acc[1] / n);
            pixels[dst + 2] = static_cast<std::uint8_t>(acc[2] / n);
            pixels[dst + 3] = static_cast<std::uint8_t>(acc[3] / n);
        }
    }
}

// ---------------- 渐变扫描线标量黄金参考（WS-4：gradient SIMD 双实现【D1】）----------------
// 与 Painter 不透明快路径位级一致：双色标（stops[0..1]）、全不透明（a=255）时 sample_gradient
// 退化为 c = c0 + (c1-c0)*frac，frac = clamp(t,0,1) 在 [stop0, stop0+range] 上的归一。
// 浮点运算序列刻意与 SSE2/AVX2 版本 1:1 对应（-ffp-contract=off，无 FMA），保证逐位一致。
// 不读取帧缓冲：输出仅取决于几何与端点色，直接写 RGB + A(=255)；整数截断用 cvtt 语义（向零截断 + 钳位）。
inline auto gradient_linear_scanline_scalar(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                            float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1,
                                            float stop0, float range) -> int {
    (void)dy; // 线性扫描线中 dy 已折叠进 py（调用方逐行预计算），此处不使用。
    for (int k = 0; k < n; ++k) {
        const float x = static_cast<float>(x0 + k);
        const float px = (x - sx) * dx + py;
        float t = px * inv_len_sq;
        t = std::max(0.0f, std::min(1.0f, t));
        const float frac = (range > 0.0f) ? (t - stop0) / range : 0.0f;
        std::uint8_t *p = row + static_cast<std::size_t>(k) * 4u;
        for (int c = 0; c < 3; ++c) {
            const float val = static_cast<float>(c0[c]) + static_cast<float>(c1[c] - c0[c]) * frac;
            int iv = static_cast<int>(val);
            if (iv < 0) {
                iv = 0;
            } else if (iv > 255) {
                iv = 255;
            }
            p[c] = static_cast<std::uint8_t>(iv);
        }
        p[3] = 255;
    }
    return n;
}

inline auto gradient_radial_scanline_scalar(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                            const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range)
    -> int {
    for (int k = 0; k < n; ++k) {
        const float x = static_cast<float>(x0 + k);
        const float px = x - cx;
        const float dist = std::sqrt(px * px + py);
        float t = dist * inv_r;
        t = std::max(0.0f, std::min(1.0f, t));
        const float frac = (range > 0.0f) ? (t - stop0) / range : 0.0f;
        std::uint8_t *p = row + static_cast<std::size_t>(k) * 4u;
        for (int c = 0; c < 3; ++c) {
            const float val = static_cast<float>(c0[c]) + static_cast<float>(c1[c] - c0[c]) * frac;
            int iv = static_cast<int>(val);
            if (iv < 0) {
                iv = 0;
            } else if (iv > 255) {
                iv = 255;
            }
            p[c] = static_cast<std::uint8_t>(iv);
        }
        p[3] = 255;
    }
    return n;
}

#if defined(AURORA_ENABLE_SIMD)

// 单像素伽马混合：v = dst_lin*inv + src_lin*alpha，结果转 sRGB。
// 标量参考：blend_srgb_over(dst, src, alpha) = linear_to_srgb(srgb_to_linear(src)*alpha + srgb_to_linear(dst)*inv)
// SIMD 逐通道 c：alpha = a[c]，src = s[c]；dv = g_srgb_to_linear[d[c]]，dsf = g_srgb_to_linear[s[c]]。

#if defined(AURORA_SIMD_X86)
inline AURORA_SSE41_TARGET AURORA_NOINLINE auto blend_srgb_over_region_sse2(std::uint8_t *px, std::uint8_t sr,
                                                                            std::uint8_t sg, std::uint8_t sb, float ar,
                                                                            float ag, float ab, int n) -> void {
    const float ds_r = srgb_to_linear(sr);
    const float ds_g = srgb_to_linear(sg);
    const float ds_b = srgb_to_linear(sb);
    const __m128 v4095 = _mm_set1_ps(static_cast<float>(AURORA_LINEAR_TO_SRGB_SIZE - 1));
    const __m128 v05 = _mm_set1_ps(0.5f);
    const __m128i v0 = _mm_setzero_si128();
    const __m128i v4095i = _mm_set1_epi32(AURORA_LINEAR_TO_SRGB_SIZE - 1);

    int i = 0;
    for (; i + 4 <= n; i += 4) {
        std::uint8_t *base = px + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t d0 = base[0 * 4 + c], d1 = base[1 * 4 + c];
            const std::uint8_t d2 = base[2 * 4 + c], d3 = base[3 * 4 + c];
            const float dl0 = srgb_to_linear(d0);
            const float dl1 = srgb_to_linear(d1);
            const float dl2 = srgb_to_linear(d2);
            const float dl3 = srgb_to_linear(d3);
            const __m128 dv = _mm_setr_ps(dl0, dl1, dl2, dl3);
            const float dsf = (c == 0 ? ds_r : (c == 1 ? ds_g : ds_b));
            const float aaf = (c == 0 ? ar : (c == 1 ? ag : ab));
            const __m128 v =
                _mm_add_ps(_mm_mul_ps(dv, _mm_set1_ps(1.0f - aaf)), _mm_mul_ps(_mm_set1_ps(dsf), _mm_set1_ps(aaf)));
            const __m128 t = _mm_add_ps(_mm_mul_ps(v, v4095), v05);
            __m128i idx = _mm_cvttps_epi32(t); // 向零截断，等同 (int)float
            idx = _mm_max_epi32(idx, v0);
            idx = _mm_min_epi32(idx, v4095i);
            alignas(16) int iarr[4];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(iarr), idx);
            base[0 * 4 + c] = linear_to_srgb_lut(iarr[0]);
            base[1 * 4 + c] = linear_to_srgb_lut(iarr[1]);
            base[2 * 4 + c] = linear_to_srgb_lut(iarr[2]);
            base[3 * 4 + c] = linear_to_srgb_lut(iarr[3]);
        }
        base[0 * 4 + 3] = 255;
        base[1 * 4 + 3] = 255;
        base[2 * 4 + 3] = 255;
        base[3 * 4 + 3] = 255;
    }
    if (i < n) {
        blend_srgb_over_region_scalar(px + static_cast<std::size_t>(i) * 4u, sr, sg, sb, ar, ag, ab, n - i);
    }
}

inline AURORA_SSE41_TARGET AURORA_NOINLINE auto blend_linear_region_sse2(std::uint8_t *px, std::uint8_t sr,
                                                                         std::uint8_t sg, std::uint8_t sb, float fa,
                                                                         float finv, int n) -> void {
    const __m128 vfa = _mm_set1_ps(fa);
    const __m128 vfinv = _mm_set1_ps(finv);
    const __m128i v0 = _mm_setzero_si128();
    const __m128i v255i = _mm_set1_epi32(255);

    int i = 0;
    for (; i + 4 <= n; i += 4) {
        std::uint8_t *base = px + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t d0 = base[0 * 4 + c], d1 = base[1 * 4 + c];
            const std::uint8_t d2 = base[2 * 4 + c], d3 = base[3 * 4 + c];
            const __m128i db = _mm_setr_epi8(static_cast<char>(d0), static_cast<char>(d1), static_cast<char>(d2),
                                             static_cast<char>(d3), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            const __m128 df = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(db));
            const float scf = static_cast<float>(c == 0 ? sr : (c == 1 ? sg : sb));
            const __m128 v = _mm_add_ps(_mm_mul_ps(df, vfinv), _mm_mul_ps(_mm_set1_ps(scf), vfa));
            __m128i o = _mm_cvttps_epi32(v);
            o = _mm_max_epi32(o, v0);
            o = _mm_min_epi32(o, v255i);
            alignas(16) int iarr[4];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(iarr), o);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
        }
        base[0 * 4 + 3] = 255;
        base[1 * 4 + 3] = 255;
        base[2 * 4 + 3] = 255;
        base[3 * 4 + 3] = 255;
    }
    if (i < n) {
        blend_linear_region_scalar(px + static_cast<std::size_t>(i) * 4u, sr, sg, sb, fa, finv, n - i);
    }
}

// 渐变扫描线 SSE2 实现（WS-4）：镜像 gradient_*_scanline_scalar 浮点序列，4 像素一组。
// 不读取帧缓冲（输出仅取决于几何与端点色），直接写入行内 RGB + A(=255)。
inline AURORA_SSE41_TARGET AURORA_NOINLINE auto gradient_linear_scanline_sse2(std::uint8_t *row, int x0, int n,
                                                                              float sx, float py, float dx, float dy,
                                                                              float inv_len_sq, const std::uint8_t *c0,
                                                                              const std::uint8_t *c1, float stop0,
                                                                              float range) -> int {
    (void)dy;
    const __m128 vsx = _mm_set1_ps(sx);
    const __m128 vdx = _mm_set1_ps(dx);
    const __m128 vpy = _mm_set1_ps(py);
    const __m128 vinv = _mm_set1_ps(inv_len_sq);
    const __m128 vstop0 = _mm_set1_ps(stop0);
    const __m128 vrange = _mm_set1_ps(range);
    const __m128 vzero = _mm_setzero_ps();
    const __m128 vone = _mm_set1_ps(1.0f);
    const __m128i vi0 = _mm_setzero_si128();
    const __m128i vi255 = _mm_set1_epi32(255);
    const __m128 vc0[3] = { _mm_set1_ps(static_cast<float>(c0[0])), _mm_set1_ps(static_cast<float>(c0[1])),
                            _mm_set1_ps(static_cast<float>(c0[2])) };
    const __m128 vdiff[3] = { _mm_set1_ps(static_cast<float>(c1[0] - c0[0])),
                              _mm_set1_ps(static_cast<float>(c1[1] - c0[1])),
                              _mm_set1_ps(static_cast<float>(c1[2] - c0[2])) };
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 vx = _mm_setr_ps(static_cast<float>(x0 + i), static_cast<float>(x0 + i + 1),
                                      static_cast<float>(x0 + i + 2), static_cast<float>(x0 + i + 3));
        const __m128 vpx = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(vx, vsx), vdx), vpy);
        __m128 t = _mm_mul_ps(vpx, vinv);
        t = _mm_max_ps(vzero, _mm_min_ps(vone, t));
        __m128 frac = (range > 0.0f) ? _mm_div_ps(_mm_sub_ps(t, vstop0), vrange) : vzero;
        std::uint8_t *base = row + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const __m128 val = _mm_add_ps(vc0[c], _mm_mul_ps(vdiff[c], frac));
            __m128i iv = _mm_cvttps_epi32(val);
            iv = _mm_max_epi32(iv, vi0);
            iv = _mm_min_epi32(iv, vi255);
            alignas(16) int iarr[4];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(iarr), iv);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
        }
        base[0 * 4 + 3] = 255;
        base[1 * 4 + 3] = 255;
        base[2 * 4 + 3] = 255;
        base[3 * 4 + 3] = 255;
    }
    if (i < n) {
        gradient_linear_scanline_scalar(row + static_cast<std::size_t>(i) * 4u, x0 + i, n - i, sx, py, dx, dy,
                                        inv_len_sq, c0, c1, stop0, range);
    }
    return i;
}

inline AURORA_SSE41_TARGET AURORA_NOINLINE auto
gradient_radial_scanline_sse2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r, const std::uint8_t *c0,
                              const std::uint8_t *c1, float stop0, float range) -> int {
    const __m128 vcx = _mm_set1_ps(cx);
    const __m128 vpy = _mm_set1_ps(py);
    const __m128 vinv_r = _mm_set1_ps(inv_r);
    const __m128 vstop0 = _mm_set1_ps(stop0);
    const __m128 vrange = _mm_set1_ps(range);
    const __m128 vzero = _mm_setzero_ps();
    const __m128 vone = _mm_set1_ps(1.0f);
    const __m128i vi0 = _mm_setzero_si128();
    const __m128i vi255 = _mm_set1_epi32(255);
    const __m128 vc0[3] = { _mm_set1_ps(static_cast<float>(c0[0])), _mm_set1_ps(static_cast<float>(c0[1])),
                            _mm_set1_ps(static_cast<float>(c0[2])) };
    const __m128 vdiff[3] = { _mm_set1_ps(static_cast<float>(c1[0] - c0[0])),
                              _mm_set1_ps(static_cast<float>(c1[1] - c0[1])),
                              _mm_set1_ps(static_cast<float>(c1[2] - c0[2])) };
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 vx = _mm_setr_ps(static_cast<float>(x0 + i), static_cast<float>(x0 + i + 1),
                                      static_cast<float>(x0 + i + 2), static_cast<float>(x0 + i + 3));
        const __m128 vpx = _mm_sub_ps(vx, vcx);
        const __m128 vdist = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(vpx, vpx), vpy));
        __m128 t = _mm_mul_ps(vdist, vinv_r);
        t = _mm_max_ps(vzero, _mm_min_ps(vone, t));
        __m128 frac = (range > 0.0f) ? _mm_div_ps(_mm_sub_ps(t, vstop0), vrange) : vzero;
        std::uint8_t *base = row + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const __m128 val = _mm_add_ps(vc0[c], _mm_mul_ps(vdiff[c], frac));
            __m128i iv = _mm_cvttps_epi32(val);
            iv = _mm_max_epi32(iv, vi0);
            iv = _mm_min_epi32(iv, vi255);
            alignas(16) int iarr[4];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(iarr), iv);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
        }
        base[0 * 4 + 3] = 255;
        base[1 * 4 + 3] = 255;
        base[2 * 4 + 3] = 255;
        base[3 * 4 + 3] = 255;
    }
    if (i < n) {
        gradient_radial_scanline_scalar(row + static_cast<std::size_t>(i) * 4u, x0 + i, n - i, cx, py, inv_r, c0, c1,
                                        stop0, range);
    }
    return i;
}

#endif // AURORA_SIMD_X86 (SSE2 implementations)

#if defined(__GNUC__) || defined(__clang__)
#if defined(AURORA_SIMD_X86)
AURORA_AVX2_TARGET AURORA_NOINLINE inline auto blend_srgb_over_region_avx2(std::uint8_t *px, std::uint8_t sr,
                                                                           std::uint8_t sg, std::uint8_t sb, float ar,
                                                                           float ag, float ab, int n) -> void {
    const float ds_r = srgb_to_linear(sr);
    const float ds_g = srgb_to_linear(sg);
    const float ds_b = srgb_to_linear(sb);
    const __m256 v4095 = _mm256_set1_ps(static_cast<float>(AURORA_LINEAR_TO_SRGB_SIZE - 1));
    const __m256 v05 = _mm256_set1_ps(0.5f);
    const __m256i v0 = _mm256_setzero_si256();
    const __m256i v4095i = _mm256_set1_epi32(AURORA_LINEAR_TO_SRGB_SIZE - 1);

    int i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint8_t *base = px + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t d0 = base[0 * 4 + c], d1 = base[1 * 4 + c];
            const std::uint8_t d2 = base[2 * 4 + c], d3 = base[3 * 4 + c];
            const std::uint8_t d4 = base[4 * 4 + c], d5 = base[5 * 4 + c];
            const std::uint8_t d6 = base[6 * 4 + c], d7 = base[7 * 4 + c];
            const float dl0 = srgb_to_linear(d0), dl1 = srgb_to_linear(d1);
            const float dl2 = srgb_to_linear(d2), dl3 = srgb_to_linear(d3);
            const float dl4 = srgb_to_linear(d4), dl5 = srgb_to_linear(d5);
            const float dl6 = srgb_to_linear(d6), dl7 = srgb_to_linear(d7);
            const __m256 dv = _mm256_setr_ps(dl0, dl1, dl2, dl3, dl4, dl5, dl6, dl7);
            const float dsf = (c == 0 ? ds_r : (c == 1 ? ds_g : ds_b));
            const float aaf = (c == 0 ? ar : (c == 1 ? ag : ab));
            const __m256 v = _mm256_add_ps(_mm256_mul_ps(dv, _mm256_set1_ps(1.0f - aaf)),
                                           _mm256_mul_ps(_mm256_set1_ps(dsf), _mm256_set1_ps(aaf)));
            const __m256 t = _mm256_add_ps(_mm256_mul_ps(v, v4095), v05);
            __m256i idx = _mm256_cvttps_epi32(t);
            idx = _mm256_max_epi32(idx, v0);
            idx = _mm256_min_epi32(idx, v4095i);
            alignas(32) int iarr[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(iarr), idx);
            base[0 * 4 + c] = linear_to_srgb_lut(iarr[0]);
            base[1 * 4 + c] = linear_to_srgb_lut(iarr[1]);
            base[2 * 4 + c] = linear_to_srgb_lut(iarr[2]);
            base[3 * 4 + c] = linear_to_srgb_lut(iarr[3]);
            base[4 * 4 + c] = linear_to_srgb_lut(iarr[4]);
            base[5 * 4 + c] = linear_to_srgb_lut(iarr[5]);
            base[6 * 4 + c] = linear_to_srgb_lut(iarr[6]);
            base[7 * 4 + c] = linear_to_srgb_lut(iarr[7]);
        }
        for (int k = 0; k < 8; ++k) {
            base[k * 4 + 3] = 255;
        }
    }
    if (i < n) {
        blend_srgb_over_region_scalar(px + static_cast<std::size_t>(i) * 4u, sr, sg, sb, ar, ag, ab, n - i);
    }
}

AURORA_AVX2_TARGET AURORA_NOINLINE inline auto blend_linear_region_avx2(std::uint8_t *px, std::uint8_t sr,
                                                                        std::uint8_t sg, std::uint8_t sb, float fa,
                                                                        float finv, int n) -> void {
    const __m256 vfa = _mm256_set1_ps(fa);
    const __m256 vfinv = _mm256_set1_ps(finv);
    const __m256i v0 = _mm256_setzero_si256();
    const __m256i v255i = _mm256_set1_epi32(255);

    int i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint8_t *base = px + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t d0 = base[0 * 4 + c], d1 = base[1 * 4 + c];
            const std::uint8_t d2 = base[2 * 4 + c], d3 = base[3 * 4 + c];
            const std::uint8_t d4 = base[4 * 4 + c], d5 = base[5 * 4 + c];
            const std::uint8_t d6 = base[6 * 4 + c], d7 = base[7 * 4 + c];
            // 8 字节无符号扩展为 8 个 float：分两组 128 位（d0..d3 与 d4..d7）各自做
            // SSE4.1 零扩展，再用 _mm256_set_m128i 拼成 8 路（低 128 位 d0..d3，高 128 位 d4..d7）。
            const __m128i lo4 =
                _mm_cvtepu8_epi32(_mm_setr_epi8(static_cast<char>(d0), static_cast<char>(d1), static_cast<char>(d2),
                                                static_cast<char>(d3), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            const __m128i hi4 =
                _mm_cvtepu8_epi32(_mm_setr_epi8(static_cast<char>(d4), static_cast<char>(d5), static_cast<char>(d6),
                                                static_cast<char>(d7), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
            const __m256i i_all = _mm256_set_m128i(hi4, lo4);
            const __m256 df = _mm256_cvtepi32_ps(i_all);
            const float scf = static_cast<float>(c == 0 ? sr : (c == 1 ? sg : sb));
            const __m256 v = _mm256_add_ps(_mm256_mul_ps(df, vfinv), _mm256_mul_ps(_mm256_set1_ps(scf), vfa));
            __m256i o = _mm256_cvttps_epi32(v);
            o = _mm256_max_epi32(o, v0);
            o = _mm256_min_epi32(o, v255i);
            alignas(32) int iarr[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(iarr), o);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
            base[4 * 4 + c] = static_cast<std::uint8_t>(iarr[4]);
            base[5 * 4 + c] = static_cast<std::uint8_t>(iarr[5]);
            base[6 * 4 + c] = static_cast<std::uint8_t>(iarr[6]);
            base[7 * 4 + c] = static_cast<std::uint8_t>(iarr[7]);
        }
        for (int k = 0; k < 8; ++k) {
            base[k * 4 + 3] = 255;
        }
    }
    if (i < n) {
        blend_linear_region_scalar(px + static_cast<std::size_t>(i) * 4u, sr, sg, sb, fa, finv, n - i);
    }
}
// 渐变扫描线 AVX2 实现（WS-4）：镜像标量浮点序列，8 像素一组。
AURORA_AVX2_TARGET AURORA_NOINLINE inline auto gradient_linear_scanline_avx2(std::uint8_t *row, int x0, int n, float sx,
                                                                             float py, float dx, float dy,
                                                                             float inv_len_sq, const std::uint8_t *c0,
                                                                             const std::uint8_t *c1, float stop0,
                                                                             float range) -> int {
    (void)dy;
    const __m256 vsx = _mm256_set1_ps(sx);
    const __m256 vdx = _mm256_set1_ps(dx);
    const __m256 vpy = _mm256_set1_ps(py);
    const __m256 vinv = _mm256_set1_ps(inv_len_sq);
    const __m256 vstop0 = _mm256_set1_ps(stop0);
    const __m256 vrange = _mm256_set1_ps(range);
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vone = _mm256_set1_ps(1.0f);
    const __m256i vi0 = _mm256_setzero_si256();
    const __m256i vi255 = _mm256_set1_epi32(255);
    const __m256 vc0[3] = { _mm256_set1_ps(static_cast<float>(c0[0])), _mm256_set1_ps(static_cast<float>(c0[1])),
                            _mm256_set1_ps(static_cast<float>(c0[2])) };
    const __m256 vdiff[3] = { _mm256_set1_ps(static_cast<float>(c1[0] - c0[0])),
                              _mm256_set1_ps(static_cast<float>(c1[1] - c0[1])),
                              _mm256_set1_ps(static_cast<float>(c1[2] - c0[2])) };
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 vx = _mm256_setr_ps(static_cast<float>(x0 + i), static_cast<float>(x0 + i + 1),
                                         static_cast<float>(x0 + i + 2), static_cast<float>(x0 + i + 3),
                                         static_cast<float>(x0 + i + 4), static_cast<float>(x0 + i + 5),
                                         static_cast<float>(x0 + i + 6), static_cast<float>(x0 + i + 7));
        const __m256 vpx = _mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(vx, vsx), vdx), vpy);
        __m256 t = _mm256_mul_ps(vpx, vinv);
        t = _mm256_max_ps(vzero, _mm256_min_ps(vone, t));
        __m256 frac = (range > 0.0f) ? _mm256_div_ps(_mm256_sub_ps(t, vstop0), vrange) : vzero;
        std::uint8_t *base = row + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const __m256 val = _mm256_add_ps(vc0[c], _mm256_mul_ps(vdiff[c], frac));
            __m256i iv = _mm256_cvttps_epi32(val);
            iv = _mm256_max_epi32(iv, vi0);
            iv = _mm256_min_epi32(iv, vi255);
            alignas(32) int iarr[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(iarr), iv);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
            base[4 * 4 + c] = static_cast<std::uint8_t>(iarr[4]);
            base[5 * 4 + c] = static_cast<std::uint8_t>(iarr[5]);
            base[6 * 4 + c] = static_cast<std::uint8_t>(iarr[6]);
            base[7 * 4 + c] = static_cast<std::uint8_t>(iarr[7]);
        }
        for (int k = 0; k < 8; ++k) {
            base[k * 4 + 3] = 255;
        }
    }
    if (i < n) {
        gradient_linear_scanline_scalar(row + static_cast<std::size_t>(i) * 4u, x0 + i, n - i, sx, py, dx, dy,
                                        inv_len_sq, c0, c1, stop0, range);
    }
    return i;
}

AURORA_AVX2_TARGET AURORA_NOINLINE inline auto
gradient_radial_scanline_avx2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r, const std::uint8_t *c0,
                              const std::uint8_t *c1, float stop0, float range) -> int {
    const __m256 vcx = _mm256_set1_ps(cx);
    const __m256 vpy = _mm256_set1_ps(py);
    const __m256 vinv_r = _mm256_set1_ps(inv_r);
    const __m256 vstop0 = _mm256_set1_ps(stop0);
    const __m256 vrange = _mm256_set1_ps(range);
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vone = _mm256_set1_ps(1.0f);
    const __m256i vi0 = _mm256_setzero_si256();
    const __m256i vi255 = _mm256_set1_epi32(255);
    const __m256 vc0[3] = { _mm256_set1_ps(static_cast<float>(c0[0])), _mm256_set1_ps(static_cast<float>(c0[1])),
                            _mm256_set1_ps(static_cast<float>(c0[2])) };
    const __m256 vdiff[3] = { _mm256_set1_ps(static_cast<float>(c1[0] - c0[0])),
                              _mm256_set1_ps(static_cast<float>(c1[1] - c0[1])),
                              _mm256_set1_ps(static_cast<float>(c1[2] - c0[2])) };
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 vx = _mm256_setr_ps(static_cast<float>(x0 + i), static_cast<float>(x0 + i + 1),
                                         static_cast<float>(x0 + i + 2), static_cast<float>(x0 + i + 3),
                                         static_cast<float>(x0 + i + 4), static_cast<float>(x0 + i + 5),
                                         static_cast<float>(x0 + i + 6), static_cast<float>(x0 + i + 7));
        const __m256 vpx = _mm256_sub_ps(vx, vcx);
        const __m256 vdist = _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(vpx, vpx), vpy));
        __m256 t = _mm256_mul_ps(vdist, vinv_r);
        t = _mm256_max_ps(vzero, _mm256_min_ps(vone, t));
        __m256 frac = (range > 0.0f) ? _mm256_div_ps(_mm256_sub_ps(t, vstop0), vrange) : vzero;
        std::uint8_t *base = row + static_cast<std::size_t>(i) * 4u;
        for (int c = 0; c < 3; ++c) {
            const __m256 val = _mm256_add_ps(vc0[c], _mm256_mul_ps(vdiff[c], frac));
            __m256i iv = _mm256_cvttps_epi32(val);
            iv = _mm256_max_epi32(iv, vi0);
            iv = _mm256_min_epi32(iv, vi255);
            alignas(32) int iarr[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(iarr), iv);
            base[0 * 4 + c] = static_cast<std::uint8_t>(iarr[0]);
            base[1 * 4 + c] = static_cast<std::uint8_t>(iarr[1]);
            base[2 * 4 + c] = static_cast<std::uint8_t>(iarr[2]);
            base[3 * 4 + c] = static_cast<std::uint8_t>(iarr[3]);
            base[4 * 4 + c] = static_cast<std::uint8_t>(iarr[4]);
            base[5 * 4 + c] = static_cast<std::uint8_t>(iarr[5]);
            base[6 * 4 + c] = static_cast<std::uint8_t>(iarr[6]);
            base[7 * 4 + c] = static_cast<std::uint8_t>(iarr[7]);
        }
        for (int k = 0; k < 8; ++k) {
            base[k * 4 + 3] = 255;
        }
    }
    if (i < n) {
        gradient_radial_scanline_scalar(row + static_cast<std::size_t>(i) * 4u, x0 + i, n - i, cx, py, inv_r, c0, c1,
                                        stop0, range);
    }
    return i;
}

#endif // AURORA_SIMD_X86 (AVX2 implementations)
#endif // GNUC/Clang

// ---------------- 整数 box blur SIMD 双实现（WS-4：blur_region 栅格原语）----------------
// 分离式两遍整数 box blur，逐位一致镜像 blur_region_scalar。
// 关键事实：窗口计数 n = 2r+1 恒定（k 循环恒跑 2r+1 次，clamp 仅改变采样源、不改计数）；
// 结果 = acc[c] / n（正整数截断）。SIMD 路径改用滑动窗口
//   S(x) = S(x-1) + src[clamp(x+r)] - src[clamp(x-1-r)]
// 整数加法结合律保证与标量全窗求和逐位一致；最终 /n 用标量整数除法，杜绝浮点误差。
// 累加器为每通道 4 路 int32（水平/垂直遍 stride 不同，故 src/dst 步长独立传入）。

#if defined(AURORA_SIMD_X86)
inline AURORA_SSE41_TARGET auto blur_load4(const std::uint8_t *p) -> __m128i {
    // 4 字节 RGBA → 4 路 int32（每通道一路）；cvtepu8 按字节解包，与端序无关。
    return _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*reinterpret_cast<const int *>(p)));
}

// 单条线（n 个 RGBA 元组）整数 box blur 的滑动窗口实现。
// src_step/dst_step = 线内相邻元素的字节步长：水平遍为 4（同行像素紧邻）；
// 垂直遍为行步长（tmp 行宽 rw*4 / 帧缓冲行宽 full_width*4）。调用方已把 src/dst 定位到正确线基址。
inline AURORA_SSE41_TARGET AURORA_NOINLINE auto box_blur_line_sse2(std::uint8_t *dst, int dst_step,
                                                                   const std::uint8_t *src, int src_step, int n, int r)
    -> void {
    const int denom = 2 * r + 1;
    alignas(16) int seed[4] = { 0, 0, 0, 0 };
    for (int k = -r; k <= r; ++k) {
        const int idx = std::clamp(k, 0, n - 1);
        const std::uint8_t *p = src + static_cast<std::size_t>(idx) * src_step;
        seed[0] += p[0];
        seed[1] += p[1];
        seed[2] += p[2];
        seed[3] += p[3];
    }
    __m128i acc = _mm_loadu_si128(reinterpret_cast<__m128i *>(seed));
    {
        alignas(16) int s[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(s), acc);
        dst[0] = static_cast<std::uint8_t>(s[0] / denom);
        dst[1] = static_cast<std::uint8_t>(s[1] / denom);
        dst[2] = static_cast<std::uint8_t>(s[2] / denom);
        dst[3] = static_cast<std::uint8_t>(s[3] / denom);
    }
    for (int x = 1; x < n; ++x) {
        const int ridx = std::clamp(x + r, 0, n - 1);
        const int lidx = std::clamp(x - 1 - r, 0, n - 1);
        __m128i rv = blur_load4(src + static_cast<std::size_t>(ridx) * src_step);
        __m128i lv = blur_load4(src + static_cast<std::size_t>(lidx) * src_step);
        acc = _mm_sub_epi32(acc, lv);
        acc = _mm_add_epi32(acc, rv);
        alignas(16) int s[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(s), acc);
        std::uint8_t *d = dst + static_cast<std::size_t>(x) * dst_step;
        d[0] = static_cast<std::uint8_t>(s[0] / denom);
        d[1] = static_cast<std::uint8_t>(s[1] / denom);
        d[2] = static_cast<std::uint8_t>(s[2] / denom);
        d[3] = static_cast<std::uint8_t>(s[3] / denom);
    }
}

inline AURORA_SSE41_TARGET AURORA_NOINLINE auto blur_region_sse2(std::uint8_t *pixels, int full_width, int x0, int y0,
                                                                 int rw, int rh, int r) -> void {
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(rw) * rh * 4);
    const int fb_stride = full_width * 4;
    const int tmp_stride = rw * 4;
    // 第一遍：水平（帧缓冲 → tmp）。同行像素紧邻，线内步长均为 4。
    for (int y = 0; y < rh; ++y) {
        std::uint8_t *dst = tmp.data() + static_cast<std::size_t>(y) * tmp_stride;
        const std::uint8_t *src = pixels + (static_cast<std::size_t>(y0 + y) * full_width + x0) * 4;
        box_blur_line_sse2(dst, 4, src, 4, rw, r);
    }
    // 第二遍：垂直（tmp → 帧缓冲）。线内步长 = 行步长（tmp 行宽 rw*4 / 帧缓冲行宽 full_width*4）。
    for (int x = 0; x < rw; ++x) {
        std::uint8_t *dst = pixels + (static_cast<std::size_t>(y0) * full_width + (x0 + x)) * 4;
        const std::uint8_t *src = tmp.data() + static_cast<std::size_t>(x) * 4;
        box_blur_line_sse2(dst, fb_stride, src, tmp_stride, rh, r);
    }
}
#endif // AURORA_SIMD_X86 (SSE2 blur)

#if defined(__GNUC__) || defined(__clang__)
#if defined(AURORA_SIMD_X86)
inline AURORA_AVX2_TARGET auto blur_load4_avx2(const std::uint8_t *p) -> __m256i {
    // 低 128 位装 4 通道，高 128 位置零（单线模式，仅用低 4 路）。
    const __m128i lo = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*reinterpret_cast<const int *>(p)));
    return _mm256_castsi128_si256(lo);
}

inline AURORA_AVX2_TARGET AURORA_NOINLINE auto box_blur_line_avx2(std::uint8_t *dst, int dst_step,
                                                                  const std::uint8_t *src, int src_step, int n, int r)
    -> void {
    const int denom = 2 * r + 1;
    alignas(32) int seed[8] = { 0 };
    for (int k = -r; k <= r; ++k) {
        const int idx = std::clamp(k, 0, n - 1);
        const std::uint8_t *p = src + static_cast<std::size_t>(idx) * src_step;
        seed[0] += p[0];
        seed[1] += p[1];
        seed[2] += p[2];
        seed[3] += p[3];
    }
    __m256i acc = _mm256_loadu_si256(reinterpret_cast<__m256i *>(seed));
    for (int x = 0; x < n; ++x) {
        if (x > 0) {
            const int ridx = std::clamp(x + r, 0, n - 1);
            const int lidx = std::clamp(x - 1 - r, 0, n - 1);
            __m256i rv = blur_load4_avx2(src + static_cast<std::size_t>(ridx) * src_step);
            __m256i lv = blur_load4_avx2(src + static_cast<std::size_t>(lidx) * src_step);
            acc = _mm256_sub_epi32(acc, lv);
            acc = _mm256_add_epi32(acc, rv);
        }
        alignas(32) int s[8];
        _mm256_storeu_si256(reinterpret_cast<__m256i *>(s), acc);
        std::uint8_t *d = dst + static_cast<std::size_t>(x) * dst_step;
        d[0] = static_cast<std::uint8_t>(s[0] / denom);
        d[1] = static_cast<std::uint8_t>(s[1] / denom);
        d[2] = static_cast<std::uint8_t>(s[2] / denom);
        d[3] = static_cast<std::uint8_t>(s[3] / denom);
    }
}

inline AURORA_AVX2_TARGET AURORA_NOINLINE auto blur_region_avx2(std::uint8_t *pixels, int full_width, int x0, int y0,
                                                                int rw, int rh, int r) -> void {
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(rw) * rh * 4);
    const int fb_stride = full_width * 4;
    const int tmp_stride = rw * 4;
    // 第一遍：水平（帧缓冲 → tmp）。线内步长均为 4。
    for (int y = 0; y < rh; ++y) {
        std::uint8_t *dst = tmp.data() + static_cast<std::size_t>(y) * tmp_stride;
        const std::uint8_t *src = pixels + (static_cast<std::size_t>(y0 + y) * full_width + x0) * 4;
        box_blur_line_avx2(dst, 4, src, 4, rw, r);
    }
    // 第二遍：垂直（tmp → 帧缓冲）。线内步长 = 行步长。
    for (int x = 0; x < rw; ++x) {
        std::uint8_t *dst = pixels + (static_cast<std::size_t>(y0) * full_width + (x0 + x)) * 4;
        const std::uint8_t *src = tmp.data() + static_cast<std::size_t>(x) * 4;
        box_blur_line_avx2(dst, fb_stride, src, tmp_stride, rh, r);
    }
}
#endif // AURORA_SIMD_X86 (AVX2 blur)
#endif // GNUC/Clang

// ---------------- 运行时探测 / 分发初始化 ----------------
inline SimdLevel g_simd_level = SimdLevel::SSE2; // x86-64 基线；detect 后可能升为 AVX2

inline auto detect_simd_level() noexcept -> SimdLevel {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2")) {
        return SimdLevel::AVX2;
    }
#endif
    return SimdLevel::SSE2; // x86-64 恒有 SSE2
#else
    return SimdLevel::Scalar; // ARM/NEON 暂缓
#endif
}

inline auto ensure_simd_init() noexcept -> void {
    static bool done = false;
    if (!done) {
        g_simd_level = detect_simd_level();
        done = true;
    }
}

#endif // AURORA_ENABLE_SIMD

} // namespace aurora::detail
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-narrowing-conversions,
// bugprone-narrowing-conversions, readability-math-missing-parentheses, cppcoreguidelines-avoid-c-arrays,
// modernize-avoid-c-arrays, cppcoreguidelines-pro-type-reinterpret-cast,
// cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-isolate-declaration, readability-avoid-nested-conditional-operator, modernize-use-auto)
