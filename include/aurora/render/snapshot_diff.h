#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "aurora/core/image.h"

namespace aurora {

/**
 * @brief 快照对比结果（规格 §7.3）。
 */
struct SnapshotDiff {
    bool size_mismatch = false;       ///< 尺寸不一致（其余字段无意义）
    std::size_t pixel_diff_count = 0; ///< 超过容差的像素数
    int max_color_delta = 0;          ///< 最大单通道差值（0..255）
    double diff_ratio = 0.0;          ///< 差异像素占比（0..1）
    Image diff_image;                 ///< 差异可视化图（差异处红色，相同处原图淡化）

    /// @brief 按阈值判定是否通过（差异占比 <= max_ratio）。
    [[nodiscard]] auto passed(double max_ratio = 0.0) const -> bool {
        return !size_mismatch && diff_ratio <= max_ratio;
    }
};

/**
 * @brief 逐像素对比两张 RGBA8 快照（规格 §7.3）。
 *
 * @param baseline  基线图
 * @param current   当前图
 * @param tolerance 单通道容差（0..255；任一 RGBA 通道差 > tolerance 记为差异像素）
 * @return 对比结果（含差异可视化图：差异像素红色、相同像素基线淡化 25%）
 *
 * 用途：CI 集成 —— golden 基线与当前渲染比对，超阈值报错；
 * CLI：`aurora-cli snapshot --compare baseline.png`。
 */
[[nodiscard]] inline auto compare_snapshots(const Image &baseline, const Image &current, int tolerance = 0)
    -> SnapshotDiff {
    SnapshotDiff out;
    if (baseline.width != current.width || baseline.height != current.height) {
        out.size_mismatch = true;
        return out;
    }
    const std::size_t total = static_cast<std::size_t>(baseline.width) * baseline.height;
    out.diff_image.width = baseline.width;
    out.diff_image.height = baseline.height;
    out.diff_image.pixels.assign(total * 4, 0);

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 所有下标访问均由 i < total 与 ch < 4 保证在已分配的 [0, total*4) 范围内；
    // 此处为逐像素快照对比热循环，使用 operator[] 避免 at() 的重复边界检查开销。
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t off = i * 4;
        int delta = 0;
        for (int ch = 0; ch < 4; ++ch) {
            delta = std::max(delta, std::abs(static_cast<int>(baseline.pixels[off + ch]) -
                                             static_cast<int>(current.pixels[off + ch])));
        }
        out.max_color_delta = std::max(out.max_color_delta, delta);
        if (delta > tolerance) {
            ++out.pixel_diff_count;
            // 差异处标红
            out.diff_image.pixels[off] = 255;
            out.diff_image.pixels[off + 1] = 0;
            out.diff_image.pixels[off + 2] = 0;
            out.diff_image.pixels[off + 3] = 255;
        } else {
            // 相同处基线淡化 25%（保留轮廓便于人工核对）
            out.diff_image.pixels[off] = static_cast<std::uint8_t>(baseline.pixels[off] / 4);
            out.diff_image.pixels[off + 1] = static_cast<std::uint8_t>(baseline.pixels[off + 1] / 4);
            out.diff_image.pixels[off + 2] = static_cast<std::uint8_t>(baseline.pixels[off + 2] / 4);
            out.diff_image.pixels[off + 3] = 255;
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    out.diff_ratio = total > 0 ? static_cast<double>(out.pixel_diff_count) / static_cast<double>(total) : 0.0;
    return out;
}

} // namespace aurora
