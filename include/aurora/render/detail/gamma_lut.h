// Aurora — gamma LUT + 标量黄金混合参考实现（供 SIMD 双实现共享）
// 标量参考实现保持现状不动，golden 以此为准。
// SIMD 路径必须与下方标量路径逐位一致（浮点运算序列镜像 + -ffp-contract=off）。
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "aurora/core/assert.h"

namespace aurora::detail {

// sRGB <-> 线性光查找表（gamma-correct alpha 混合用）。
constexpr int AURORA_LINEAR_TO_SRGB_SIZE = 4096;

// 三张表聚合成单一结构 + C++17 inline 变量，替代原先的 extern 全局数组声明：
//  - inline 变量由链接器保证跨 TU 单一对象，语义等同 extern，但无需「头声明 + .cpp 定义」两段式；
//  - 结构值初始化（{} / ready{false}）为常量初始化，无跨 TU 动态初始化顺序问题；
//  - srgb_to_linear()/linear_to_srgb() 为逐像素热路径，inline 变量可被直接内联索引，
//    零 guard 开销（刻意不用 Meyers 单例，避免函数局部 static 的 guard 检查）。
struct GammaTables {
    std::array<float, 256> srgb_to_linear{};
    std::array<std::uint8_t, AURORA_LINEAR_TO_SRGB_SIZE> linear_to_srgb{};
    std::atomic<bool> ready{false};
};
inline GammaTables g_gamma_tables{};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

auto init_gamma_tables() -> void;

inline auto srgb_to_linear(std::uint8_t v) -> float {
    // 索引类型 uint8_t 的取值域 [0,255] 与表长 256 完全一致，运行期越界不可能发生。
    // 这里是全仓 LUT 直查的**唯一可信点**：调用方一律走本函数，不再各自直接下标。
    return g_gamma_tables.srgb_to_linear[v];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,
                                              // cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

inline auto linear_to_srgb(float v) -> std::uint8_t {
    const int idx = std::clamp(static_cast<int>(std::lroundf(v * static_cast<float>(AURORA_LINEAR_TO_SRGB_SIZE - 1))),
                               0, AURORA_LINEAR_TO_SRGB_SIZE - 1);
    // idx 已被 clamp 夹取到 [0, SIZE-1]，无需运行期检查。
    return g_gamma_tables.linear_to_srgb[idx];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,
                                                // cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief 按整型索引取 sRGB 表值（SIMD 端专用：cvtt 截断 + min/max 夹取后取表）。
/// @note 前置条件：idx ∈ [0, AURORA_LINEAR_TO_SRGB_SIZE-1]；Debug 下由断言复核，
///       Release 下零开销直查（热路径逐像素调用，边界检查是纯开销）。
inline auto linear_to_srgb_lut(int idx) -> std::uint8_t {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while, readability-simplify-boolean-expr)
    AURORA_ASSERT(idx >= 0 && idx < AURORA_LINEAR_TO_SRGB_SIZE, "sRGB LUT index out of range");
    return g_gamma_tables.linear_to_srgb[idx];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index,
                                                // cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

// Gamma-correct source-over（单像素黄金参考）：颜色在线性光空间混合，结果转回 sRGB。
// 注意 static_cast<int>(float) 是向零截断（正值为 floor），SIMD 端必须用 cvtt 系列对应。
inline auto blend_srgb_over(std::uint8_t dst, std::uint8_t src, float alpha) -> std::uint8_t {
    const float inv = 1.0F - alpha;
    return linear_to_srgb((srgb_to_linear(src) * alpha) + (srgb_to_linear(dst) * inv));
}

}  // namespace aurora::detail