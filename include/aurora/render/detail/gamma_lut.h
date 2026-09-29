// Aurora — gamma LUT + 标量黄金混合参考实现（供 SIMD 双实现共享）
// 标量参考实现保持现状不动，golden 以此为准。
// SIMD 路径必须与下方标量路径逐位一致（浮点运算序列镜像 + -ffp-contract=off）。
#pragma once

/// @brief sRGB 与线性光查找表尺寸与门面：供 SIMD 双实现共享，标量参考实现即 golden。
/// @file

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "aurora/core/aurora_assert.h"

namespace aurora::detail {

/// @brief sRGB <-> 线性光查找表长度（线性输入按 12 位量化，覆盖 4096 档）。
constexpr int AURORA_LINEAR_TO_SRGB_SIZE = 4096;

// 三张表聚合成单一结构 + C++17 inline 变量，替代原先的 extern 全局数组声明：
//  - inline 变量由链接器保证跨 TU 单一对象，语义等同 extern，但无需「头声明 + .cpp 定义」两段式；
//  - 结构值初始化（{} / ready{false}）为常量初始化，无跨 TU 动态初始化顺序问题；
//  - srgb_to_linear()/linear_to_srgb() 为逐像素热路径，inline 变量可被直接内联索引，
//    零 guard 开销（刻意不用 Meyers 单例，避免函数局部 static 的 guard 检查）。
/// @brief gamma 查找表聚合体：sRGB→linear（8 位入）与 linear→sRGB（12 位量化入）两表 + 就绪标志。
struct GammaTables {
    std::array<float, 256> srgb_to_linear{};  ///< 8 位 sRGB → 线性光浮点（[0,1]）逐值反 gamma 表
    std::array<std::uint8_t, AURORA_LINEAR_TO_SRGB_SIZE>
        linear_to_srgb{};  ///< 12 位量化线性光 → 8 位 sRGB 正向 gamma 表
    std::atomic<bool> ready{false};  ///< 表是否已由 `init_gamma_tables` 填充；未就绪时读取会返回零值
};
/// @brief 进程级 gamma 表容器（inline 变量：链接器保证跨 TU 单一实例；常量初始化，无动态 init 顺序问题）。
inline GammaTables g_gamma_tables{};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// @brief 初始化 gamma 查找表：填充 srgb_to_linear / linear_to_srgb 并置 ready 标志。
/// @note 应在渲染热路径启动前调用一次；重复调用为幂等（依赖 ready 判定）。
auto init_gamma_tables() -> void;

/// @brief 8 位 sRGB 值转线性光浮点：热路径逐像素调用，表长 256 与 uint8_t 值域完全一致，无越界。
/// @param v 8 位 sRGB 分量（0..255）。
/// @return 对应线性光浮点（[0,1]）；调用方须保证 `init_gamma_tables` 已执行。
inline auto srgb_to_linear(std::uint8_t v) -> float {
    // 索引类型 uint8_t 的取值域 [0,255] 与表长 256 完全一致，运行期越界不可能发生。
    // 这里是全仓 LUT 直查的**唯一可信点**：调用方一律走本函数，不再各自直接下标。
    // NOLINTNEXTLINE(*-pro-bounds-*)
    return g_gamma_tables.srgb_to_linear[v];
}

/// @brief 线性光浮点转 8 位 sRGB：按 12 位量化索引查表，向零截断 + clamp。
/// @param v 线性光分量（[0,1]）；越界会被 clamp 到 [0,1]。
/// @return 8 位 sRGB 分量；SIMD 端必须复用同一 clamp/round 序列以逐位对齐。
inline auto linear_to_srgb(float v) -> std::uint8_t {
    const int idx = std::clamp(static_cast<int>(std::lroundf(v * static_cast<float>(AURORA_LINEAR_TO_SRGB_SIZE - 1))),
                               0, AURORA_LINEAR_TO_SRGB_SIZE - 1);
    // idx 已被 clamp 夹取到 [0, SIZE-1]，无需运行期检查。
    // NOLINTNEXTLINE(*-pro-bounds-*)
    return g_gamma_tables.linear_to_srgb[idx];
}

/// @brief 按整型索引取 sRGB 表值（SIMD 端专用：cvtt 截断 + min/max 夹取后取表）。
/// @param idx 已 clamp 的 12 位量化索引（∈ [0, AURORA_LINEAR_TO_SRGB_SIZE-1]）。
/// @return 对应 8 位 sRGB 分量。
/// @note 前置条件：idx ∈ [0, AURORA_LINEAR_TO_SRGB_SIZE-1]；Debug 下由断言复核，
///       Release 下零开销直查（热路径逐像素调用，边界检查是纯开销）。
inline auto linear_to_srgb_lut(int idx) -> std::uint8_t {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while, readability-simplify-boolean-expr)
    AURORA_ASSERT(idx >= 0 && idx < AURORA_LINEAR_TO_SRGB_SIZE, "sRGB LUT index out of range");
    // NOLINTNEXTLINE(*-pro-bounds-*)
    return g_gamma_tables.linear_to_srgb[idx];
}

/// @brief Gamma-correct source-over（单像素黄金参考）：颜色在线性光空间混合，结果转回 sRGB。
/// @param dst 目标 8 位 sRGB 分量（已绘制像素）。
/// @param src 源 8 位 sRGB 分量（新覆盖内容）。
/// @param alpha 源透明度（[0,1]）；1 = 完全覆盖，0 = 保留目标。
/// @return 混合后的 8 位 sRGB 分量。
/// @note static_cast<int>(float) 是向零截断（正值为 floor），SIMD 端必须用 cvtt 系列对应。
inline auto blend_srgb_over(std::uint8_t dst, std::uint8_t src, float alpha) -> std::uint8_t {
    const float inv = 1.0F - alpha;
    return linear_to_srgb((srgb_to_linear(src) * alpha) + (srgb_to_linear(dst) * inv));
}

}  // namespace aurora::detail