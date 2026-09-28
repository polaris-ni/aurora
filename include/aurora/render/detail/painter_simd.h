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

#include "aurora/core/platform.h"
#include "aurora/render/detail/gamma_lut.h"

/// @brief 光栅内核 SIMD 双实现接口：标量黄金参考与显式 SSE2/AVX2 实现的声明，以及运行时分发入口。
/// @file
namespace aurora::detail {

/// @brief SIMD 能力档位：Scalar 纯标量黄金路径，SSE2 x86-64 基线档位，AVX2 八字节通道档位；分发按运行时探测择档。
enum class SimdLevel : std::uint8_t { Scalar, SSE2, AVX2 };

/// @brief x86 架构判定宏：SSE2/AVX2 内置函数与 target 属性仅 x86 可用；非 x86（ARM/NEON）不定义，分发只走标量黄金路径。
/// 定义 AURORA_SIMD_X86 供 .inl / 测试统一判定。
/// 架构判定一律走 core/platform.h 的规范化宏（AURORA_ARCH_*），不直接书写原生架构宏
/// （见 06-app-platform.md §12.1 与 tools/check/check_platform_macros.py 守护）。
#if defined(AURORA_ARCH_X64) || defined(AURORA_ARCH_X86)
#define AURORA_SIMD_X86 1  // NOLINT(*-macro-usage)
#endif

/// @brief 运行时探测当前 CPU 的 SIMD 能力档位：x86-64 至少 SSE2，AVX2 须经 CPUID 探测（含 OS 状态保存检查）；非 x86
/// 返回 Scalar。
/// @return 探测到的能力档位。
auto detect_simd_level() noexcept -> SimdLevel;

/// @brief 懒初始化：首次调用时探测一次并把结果缓存进 g_simd_level，之后为空操作（首次混合前调用）。
auto ensure_simd_init() noexcept -> void;

/// @brief 进程级 SIMD 能力级别（懒探测一次，见 ensure_simd_init()）。
/// inline 变量替代「extern 声明 + 外部定义」两段式：链接器保证跨 TU 单一对象，
/// 且 dispatch 热路径 switch(g_simd_level) 可直读，零间接开销；刻意不包访问器，
/// 避免热路径函数调用。非 SIMD 构建下该变量无引用者（仅一字节枚举，无副作用）。
inline SimdLevel g_simd_level = SimdLevel::SSE2;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): x86-64
                                                  // 基线；运行时 detect 后可能升为 AVX2

// ---- 标量黄金参考（与现有像素逐位一致）----
/// @brief 标量黄金参考：伽马混合逐通道 alpha（ar/ag/ab），覆盖文本 AA 的 per-channel 覆盖率。
/// @param px 像素段首指针；每像素 4 字节，RGB 写回混合结果、A 写 255。
/// @param sr 源 8 位 sRGB 红分量。
/// @param sg 源 8 位 sRGB 绿分量。
/// @param sb 源 8 位 sRGB 蓝分量。
/// @param ar 红色通道覆盖率（[0,1]）。
/// @param ag 绿色通道覆盖率（[0,1]）。
/// @param ab 蓝色通道覆盖率（[0,1]）。
/// @param n  像素个数。
auto blend_srgb_over_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                   float ag, float ab, int n) -> void;
/// @brief 标量黄金参考：线性空间混合 n 个连续像素，结果 = px*finv + src*fa 向零截断，A 写 255。
/// @param px 像素段首指针；每像素 4 字节。
/// @param sr 源 8 位红分量。
/// @param sg 源 8 位绿分量。
/// @param sb 源 8 位蓝分量。
/// @param fa 源色权重。
/// @param finv 目标色权重（通常 1 - fa）。
/// @param n  像素个数。
auto blend_linear_region_scalar(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                float finv, int n) -> void;

/// @brief 整数 box blur（分离式两遍）标量黄金参考；与 Painter::blur_region 逐位一致。
/// @param pixels 帧缓冲指针（步长 full_width*4 字节）。
/// @param full_width 帧缓冲宽（像素）。
/// @param x0 区域左上角 x。
/// @param y0 区域左上角 y。
/// @param rw 区域宽（像素）。
/// @param rh 区域高（像素）。
/// @param r 窗口半径；计数 n = 2r+1 恒定（越界侧 clamp 重复采样不改计数），结果 = acc[c]/n 正整数截断。
auto blur_region_scalar(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;

/// @brief 渐变扫描线标量黄金参考（单行，真·逐像素公式，与 SIMD 逐位一致）；不透明双色标线性渐变。
/// 浮点运算序列刻意与 SIMD 版本逐位一致（-ffp-contract=off，无 FMA；sqrt/min/max/div/截断 1:1 映射）。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param sx 渐变起点 x 坐标。
/// @param py 逐行预折叠的投影常量（(y-sy)*dy，dy 已并入其中）。
/// @param dx 渐变方向向量 x 分量。
/// @param dy 渐变方向向量 y 分量；本函数不使用（已折叠进 py）。
/// @param inv_len_sq 渐变向量长度平方的倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return 已填充像素数（标量路径填完整行，恒为 n）。
auto gradient_linear_scanline_scalar(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                     float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                     float range) -> int;
/// @brief 径向渐变扫描线标量黄金参考（单行）：t = sqrt((x-cx)^2 + py) * inv_r，其余与线性版同式。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param cx 径向圆心 x 坐标。
/// @param py 逐行预计算的圆心纵向距离平方（(y-cy)^2）。
/// @param inv_r 半径倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return 已填充像素数（标量路径填完整行，恒为 n）。
auto gradient_radial_scanline_scalar(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                     const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;

// ---- 显式 SIMD 实现（供 test_simd_parity 直接比对）----
#ifdef AURORA_ENABLE_SIMD
/// @brief 显式 SSE2 实现：逐通道伽马混合的标量黄金逐位镜像，4 像素一组，尾部回落标量。
/// @param px 像素段首指针；每像素 4 字节，RGB 写回混合结果、A 写 255。
/// @param sr 源 8 位 sRGB 红分量。
/// @param sg 源 8 位 sRGB 绿分量。
/// @param sb 源 8 位 sRGB 蓝分量。
/// @param ar 红色通道覆盖率（[0,1]）。
/// @param ag 绿色通道覆盖率（[0,1]）。
/// @param ab 蓝色通道覆盖率（[0,1]）。
/// @param n  像素个数。
auto blend_srgb_over_region_sse2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                 float ag, float ab, int n) -> void;
/// @brief 显式 SSE2 实现：线性空间混合的标量黄金逐位镜像，4 像素一组，尾部回落标量。
/// @param px 像素段首指针；每像素 4 字节。
/// @param sr 源 8 位红分量。
/// @param sg 源 8 位绿分量。
/// @param sb 源 8 位蓝分量。
/// @param fa 源色权重。
/// @param finv 目标色权重（通常 1 - fa）。
/// @param n  像素个数。
auto blend_linear_region_sse2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa, float finv,
                              int n) -> void;
/// @brief 显式 AVX2 实现：逐通道伽马混合的标量黄金逐位镜像，8 像素一组，尾部回落标量。
/// @param px 像素段首指针；每像素 4 字节，RGB 写回混合结果、A 写 255。
/// @param sr 源 8 位 sRGB 红分量。
/// @param sg 源 8 位 sRGB 绿分量。
/// @param sb 源 8 位 sRGB 蓝分量。
/// @param ar 红色通道覆盖率（[0,1]）。
/// @param ag 绿色通道覆盖率（[0,1]）。
/// @param ab 蓝色通道覆盖率（[0,1]）。
/// @param n  像素个数。
auto blend_srgb_over_region_avx2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                 float ag, float ab, int n) -> void;
/// @brief 显式 AVX2 实现：线性空间混合的标量黄金逐位镜像，8 像素一组，尾部回落标量。
/// @param px 像素段首指针；每像素 4 字节。
/// @param sr 源 8 位红分量。
/// @param sg 源 8 位绿分量。
/// @param sb 源 8 位蓝分量。
/// @param fa 源色权重。
/// @param finv 目标色权重（通常 1 - fa）。
/// @param n  像素个数。
auto blend_linear_region_avx2(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa, float finv,
                              int n) -> void;
/// @brief 显式 SSE2 实现：分离式两遍整数 box blur，滑动窗口等式保证与标量黄金逐位一致。
/// @param pixels 帧缓冲指针（步长 full_width*4 字节）。
/// @param full_width 帧缓冲宽（像素）。
/// @param x0 区域左上角 x。
/// @param y0 区域左上角 y。
/// @param rw 区域宽（像素）。
/// @param rh 区域高（像素）。
/// @param r 窗口半径；计数 n = 2r+1 恒定，最终 /n 走标量整数除法。
auto blur_region_sse2(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;
/// @brief 显式 AVX2 实现：分离式两遍整数 box blur，同 SSE2 版逐位镜像标量黄金（累加器仅用低 4 路）。
/// @param pixels 帧缓冲指针（步长 full_width*4 字节）。
/// @param full_width 帧缓冲宽（像素）。
/// @param x0 区域左上角 x。
/// @param y0 区域左上角 y。
/// @param rw 区域宽（像素）。
/// @param rh 区域高（像素）。
/// @param r 窗口半径；计数 n = 2r+1 恒定，最终 /n 走标量整数除法。
auto blur_region_avx2(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void;
/// @brief 显式 SSE2 实现：线性渐变扫描线的标量黄金逐位镜像，4 像素一组。
/// @param row 该行 x0 处首字节指针；直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param sx 渐变起点 x 坐标。
/// @param py 逐行预折叠的投影常量（(y-sy)*dy，dy 已并入其中）。
/// @param dx 渐变方向向量 x 分量。
/// @param dy 渐变方向向量 y 分量；本函数不使用（已折叠进 py）。
/// @param inv_len_sq 渐变向量长度平方的倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return SIMD 已填充的像素数（4 的整数倍，可为 0）；余下尾部由调用方以标量黄金补齐。
auto gradient_linear_scanline_sse2(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                   float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                   float range) -> int;
/// @brief 显式 AVX2 实现：线性渐变扫描线的标量黄金逐位镜像，8 像素一组。
/// @param row 该行 x0 处首字节指针；直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param sx 渐变起点 x 坐标。
/// @param py 逐行预折叠的投影常量（(y-sy)*dy，dy 已并入其中）。
/// @param dx 渐变方向向量 x 分量。
/// @param dy 渐变方向向量 y 分量；本函数不使用（已折叠进 py）。
/// @param inv_len_sq 渐变向量长度平方的倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return SIMD 已填充的像素数（8 的整数倍，可为 0）；余下尾部由调用方以标量黄金补齐。
auto gradient_linear_scanline_avx2(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                   float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                   float range) -> int;
/// @brief 显式 SSE2 实现：径向渐变扫描线的标量黄金逐位镜像（_mm_sqrt_ps），4 像素一组。
/// @param row 该行 x0 处首字节指针；直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param cx 径向圆心 x 坐标。
/// @param py 逐行预计算的圆心纵向距离平方（(y-cy)^2）。
/// @param inv_r 半径倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return SIMD 已填充的像素数（4 的整数倍，可为 0）；余下尾部由调用方以标量黄金补齐。
auto gradient_radial_scanline_sse2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                   const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;
/// @brief 显式 AVX2 实现：径向渐变扫描线的标量黄金逐位镜像（_mm256_sqrt_ps），8 像素一组。
/// @param row 该行 x0 处首字节指针；直接写 RGB + A(=255)，不读帧缓冲。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param cx 径向圆心 x 坐标。
/// @param py 逐行预计算的圆心纵向距离平方（(y-cy)^2）。
/// @param inv_r 半径倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
/// @return SIMD 已填充的像素数（8 的整数倍，可为 0）；余下尾部由调用方以标量黄金补齐。
auto gradient_radial_scanline_avx2(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                   const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> int;
#endif

// ---- 分发入口（生产路径调用）----
#ifdef AURORA_ENABLE_SIMD
/// @brief 逐通道 alpha 伽马混合分发入口（生产路径调用）：确保 gamma 表与 SIMD 档位已初始化后按 g_simd_level 择档。
/// @param px 像素段首指针；每像素 4 字节，RGB 写回混合结果、A 写 255。
/// @param sr 源 8 位 sRGB 红分量。
/// @param sg 源 8 位 sRGB 绿分量。
/// @param sb 源 8 位 sRGB 蓝分量。
/// @param ar 红色通道覆盖率（[0,1]）。
/// @param ag 绿色通道覆盖率（[0,1]）。
/// @param ab 蓝色通道覆盖率（[0,1]）。
/// @param n  像素个数。
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
/// @brief 线性空间混合分发入口（生产路径调用）：确保 SIMD 档位初始化后按 g_simd_level 择档，未命中回落标量黄金。
/// @param px 像素段首指针；每像素 4 字节。
/// @param sr 源 8 位红分量。
/// @param sg 源 8 位绿分量。
/// @param sb 源 8 位蓝分量。
/// @param fa 源色权重。
/// @param finv 目标色权重（通常 1 - fa）。
/// @param n  像素个数。
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

/// @brief 整数 box blur 分发入口（生产路径调用）：确保 SIMD 档位初始化后按 g_simd_level 择档，SIMD 与标量逐位一致。
/// @param pixels 帧缓冲指针（步长 full_width*4 字节）。
/// @param full_width 帧缓冲宽（像素）。
/// @param x0 区域左上角 x。
/// @param y0 区域左上角 y。
/// @param rw 区域宽（像素）。
/// @param rh 区域高（像素）。
/// @param r 窗口半径；计数 n = 2r+1 恒定。
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

/// @brief 线性渐变扫描线分发入口（生产路径调用）：SIMD 填充向量宽度整数倍像素，尾部由标量黄金补齐；
/// SIMD 与标量逐位一致，故整行结果等同标量黄金。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param sx 渐变起点 x 坐标。
/// @param py 逐行预折叠的投影常量（(y-sy)*dy，dy 已并入其中）。
/// @param dx 渐变方向向量 x 分量。
/// @param dy 渐变方向向量 y 分量；扫描线实现不使用（已折叠进 py）。
/// @param inv_len_sq 渐变向量长度平方的倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
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
/// @brief 径向渐变扫描线分发入口（生产路径调用）：SIMD
/// 填充向量宽度整数倍像素，尾部由标量黄金补齐，整行结果等同标量黄金。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param cx 径向圆心 x 坐标。
/// @param py 逐行预计算的圆心纵向距离平方（(y-cy)^2）。
/// @param inv_r 半径倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
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
/// @brief 非 SIMD 构建下的逐通道 alpha 伽马混合入口：直接调用标量黄金实现。
/// @param px 像素段首指针；每像素 4 字节，RGB 写回混合结果、A 写 255。
/// @param sr 源 8 位 sRGB 红分量。
/// @param sg 源 8 位 sRGB 绿分量。
/// @param sb 源 8 位 sRGB 蓝分量。
/// @param ar 红色通道覆盖率（[0,1]）。
/// @param ag 绿色通道覆盖率（[0,1]）。
/// @param ab 蓝色通道覆盖率（[0,1]）。
/// @param n  像素个数。
inline auto blend_srgb_over_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float ar,
                                   float ag, float ab, int n) -> void {
    blend_srgb_over_region_scalar(px, sr, sg, sb, ar, ag, ab, n);
}
/// @brief 非 SIMD 构建下的线性混合入口：直接调用标量黄金实现。
/// @param px 像素段首指针；每像素 4 字节。
/// @param sr 源 8 位红分量。
/// @param sg 源 8 位绿分量。
/// @param sb 源 8 位蓝分量。
/// @param fa 源色权重。
/// @param finv 目标色权重（通常 1 - fa）。
/// @param n  像素个数。
inline auto blend_linear_region(std::uint8_t *px, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, float fa,
                                float finv, int n) -> void {
    blend_linear_region_scalar(px, sr, sg, sb, fa, finv, n);
}

/// @brief 非 SIMD 构建下的整数 box blur 入口：直接调用标量黄金实现。
/// @param pixels 帧缓冲指针（步长 full_width*4 字节）。
/// @param full_width 帧缓冲宽（像素）。
/// @param x0 区域左上角 x。
/// @param y0 区域左上角 y。
/// @param rw 区域宽（像素）。
/// @param rh 区域高（像素）。
/// @param r 窗口半径；计数 n = 2r+1 恒定。
inline auto blur_region(std::uint8_t *pixels, int full_width, int x0, int y0, int rw, int rh, int r) -> void {
    blur_region_scalar(pixels, full_width, x0, y0, rw, rh, r);
}

/// @brief 非 SIMD 构建下的线性渐变扫描线入口：直接调用标量黄金实现填完整行。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param sx 渐变起点 x 坐标。
/// @param py 逐行预折叠的投影常量（(y-sy)*dy，dy 已并入其中）。
/// @param dx 渐变方向向量 x 分量。
/// @param dy 渐变方向向量 y 分量；扫描线实现不使用（已折叠进 py）。
/// @param inv_len_sq 渐变向量长度平方的倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
inline auto gradient_linear_fill(std::uint8_t *row, int x0, int n, float sx, float py, float dx, float dy,
                                 float inv_len_sq, const std::uint8_t *c0, const std::uint8_t *c1, float stop0,
                                 float range) -> void {
    gradient_linear_scanline_scalar(row, x0, n, sx, py, dx, dy, inv_len_sq, c0, c1, stop0, range);
}
/// @brief 非 SIMD 构建下的径向渐变扫描线入口：直接调用标量黄金实现填完整行。
/// @param row 该行 x0 处首字节指针；填充 [x0, x0+n) 共 n 个像素，直接写 RGB + A(=255)。
/// @param x0 首像素的 x 坐标。
/// @param n  像素个数。
/// @param cx 径向圆心 x 坐标。
/// @param py 逐行预计算的圆心纵向距离平方（(y-cy)^2）。
/// @param inv_r 半径倒数。
/// @param c0 起点色 RGB 三字节指针。
/// @param c1 终点色 RGB 三字节指针。
/// @param stop0 首色标的归一化停止位置。
/// @param range 两色标间跨度（<=0 时插值系数取 0）。
inline auto gradient_radial_fill(std::uint8_t *row, int x0, int n, float cx, float py, float inv_r,
                                 const std::uint8_t *c0, const std::uint8_t *c1, float stop0, float range) -> void {
    gradient_radial_scanline_scalar(row, x0, n, cx, py, inv_r, c0, c1, stop0, range);
}
#endif

}  // namespace aurora::detail