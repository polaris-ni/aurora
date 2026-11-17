#pragma once

#include <cstddef>

// 库内部头（实现侧，非公共 API）：仅供测试观测布局中间量缓冲的复用状态。
// 测试 TU 以 `src/` 为附加包含根，见 tests/AuroraTests.cmake。

namespace aurora::detail {

/// @brief 布局中间量缓冲的观测快照（只读，供测试断言复用行为）。
struct FlexScratchStats {
    std::size_t layers = 0;              ///< 当前线程常驻的缓冲层数
    std::size_t sizes_capacity = 0;      ///< 指定层 `sizes` 的容量（层不存在时为 0）
    std::size_t baselines_capacity = 0;  ///< 指定层 `baselines` 的容量（层不存在时为 0）
};

/// @brief 读取指定深度缓冲层的复用状态。
/// @param slot 缓冲层下标（0 = 最外层布局）。
/// @return 层数与两项容量；`slot` 越界时两项容量为 0。
auto flex_scratch_stats(std::size_t slot) -> FlexScratchStats;

}  // namespace aurora::detail
