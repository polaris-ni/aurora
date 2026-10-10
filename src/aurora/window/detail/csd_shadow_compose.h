// csd_shadow_compose.h —— 软件 wl_shm 上屏路径的阴影 margin 合成器（仅库实现可见）。
//
// 为何不经过 Painter：Painter 的全局不变量是「目标画布不透明」——set_pixel 与
// fill_rect_fast_path 的所有混合都恒写 alpha=255（见 painter.cpp），在 CSD margin 的
// 零基底上会把柔影染成不透明黑边。wl_shm ARGB8888 缓冲需要的是逐像素真 alpha，故在
// present() 的 swizzle 之后，由本合成器直接改写 margin 环的 BGRA（预乘）字。
//
// 视觉同源：基色、模糊内缩与衰减因子全部取自 csd_geometry.h，GPU 路径的 Shadow 命令
// （WgpuRhi fs_shadow）与本合成器共用同一份参数与 `shadow_attenuation` 公式。
#pragma once

#include <cmath>
#include <cstdint>

#include "aurora/core/color.h"
#include "aurora/window/detail/csd_geometry.h"

namespace aurora::csd {

// NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)

/// @brief 计算一个 RGBA 直色像素的 BGRA 预乘字（小端内存 B,G,R,A）。
///
/// 与 `swizzle_rgba_premul_to_bgra` 的逐像素算术一致（`(c*a+255)>>8` 整数近似）；
/// 本合成器逐像素落字，单字内联版避免为每个 margin 像素构造临时数组。
/// @param c 直色（已乘衰减后的 alpha 落在 @p a）。
/// @param a 最终覆盖 alpha（0..255）。
/// @return wl_shm ARGB8888 缓冲中的 32 位字。
[[nodiscard]] inline auto premul_bgra_word(Color c, std::uint8_t a) -> std::uint32_t {
    const auto r = static_cast<std::uint32_t>(c.r);
    const auto g = static_cast<std::uint32_t>(c.g);
    const auto b = static_cast<std::uint32_t>(c.b);
    const auto aa = static_cast<std::uint32_t>(a);
    const std::uint32_t rp = ((r * aa) + 0xFFU) >> 8;
    const std::uint32_t gp = ((g * aa) + 0xFFU) >> 8;
    const std::uint32_t bp = ((b * aa) + 0xFFU) >> 8;
    return (aa << 24U) | (rp << 16U) | (gp << 8U) | bp;
}

/// @brief 把窗口阴影合成进 wl_shm 映射缓冲的 margin 环（内容矩形字保持原样不动）。
///
/// 坐标全部为**设备物理像素**（逻辑 dp × wl_surface buffer scale）。内容矩形
/// `[cx0, cx0+cw) × [cy0, cy0+ch)` 之外的整圈边带逐像素按到内容矩形的欧氏距离衰减，
/// 角部两向距离组合（与 GPU SDF 盒同构）；内容矩形（含其边界行）一律跳过，由
/// Painter→swizzle 产出的不透明内容保持上屏不变。
///
/// 幂等：同一缓冲重复合成结果相同（margin 字被整体覆写，含衰减为 0 的透明字 0），
/// 故每帧全量/增量 swizzle 之后都可无条件调用。
/// @param dst 已映射的 wl_shm ARGB8888 缓冲首指针（phys_w*phys_h 个 32 位字）。
/// @param phys_w 缓冲物理宽。
/// @param phys_h 缓冲物理高。
/// @param cx0 内容矩形物理左边界（= margin_px）。
/// @param cy0 内容矩形物理上边界（= margin_px）。
/// @param cw 内容矩形物理宽。
/// @param ch 内容矩形物理高。
/// @param margin_px margin 物理厚度（内容矩形到缓冲边缘的距离）。
/// @param blur_px 衰减模糊半径（物理像素；典型 = margin_px − AURORA_SHADOW_BLUR_INSET_PX*scale）。
/// @param base 阴影基色（alpha 为近内容边的不透明度）。
inline auto compose_shadow_margins_bgra(std::uint32_t *dst, int phys_w, int phys_h, int cx0, int cy0, int cw, int ch,
                                        int margin_px, float blur_px, Color base) -> void {
    if (dst == nullptr || margin_px <= 0 || cw <= 0 || ch <= 0) {
        return;
    }
    const int cx1 = cx0 + cw;  // 内容右开边界
    const int cy1 = cy0 + ch;

    // 到内容矩形最近覆盖列/行的外延距离（首条外延列 = 1，四向对称；内容内部为 0 跳过）。
    auto outside_dist = [](int p, int e0, int e1) -> int {
        if (p < e0) {
            return e0 - p;
        }
        if (p >= e1) {
            return p - (e1 - 1);
        }
        return 0;
    };

    auto write_band = [&](int x0, int y0, int x1, int y1) -> void {
        for (int y = y0; y < y1; ++y) {
            const int dy = outside_dist(y, cy0, cy1);
            std::uint32_t *row = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(phys_w));
            for (int x = x0; x < x1; ++x) {
                const int dx = outside_dist(x, cx0, cx1);
                if (dx == 0 && dy == 0) {
                    continue;  // 内容矩形：不触
                }
                const float dist = std::sqrt(static_cast<float>((dx * dx) + (dy * dy)));
                const float f = shadow_attenuation(dist, blur_px);
                if (f <= 0.0F) {
                    row[x] = 0U;  // 衰减已尽：透明字（同时清掉槽内上帧残留）
                    continue;
                }
                const auto a = static_cast<std::uint8_t>(static_cast<float>(base.a) * f);
                row[x] = premul_bgra_word(base, a);
            }
        }
    };

    // 左/右两条整高带（含四角），顶/底只写两带之间的中段——每像素恰写一次。
    write_band(0, 0, cx0, phys_h);
    write_band(cx1, 0, phys_w, phys_h);
    write_band(cx0, 0, cx1, cy0);
    write_band(cx0, cy1, cx1, phys_h);
}

// NOLINTEND(*-pro-bounds-pointer-arithmetic)

}  // namespace aurora::csd
