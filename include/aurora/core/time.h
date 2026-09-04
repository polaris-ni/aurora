#pragma once

#include <aurora/core/debug.h>

#include <chrono>
#include <cstdint>

namespace aurora {

/**
 * @brief 当前时间戳（specification/01-core.md §7）。
 *
 * 返回系统时钟的毫秒级时间戳，用于日志 / 快照标注。**注意**：视图渲染的实时逻辑
 * 仍应从 `State`/`Signal` 显式传入时间，避免视图直接读可变全局时钟（见 `debug::check_render_purity`）。
 */
[[nodiscard]] inline auto current_timestamp() -> std::uint64_t {
#ifdef AURORA_ENABLE_DEBUG
    // 渲染纯度守卫：声明式 UI 的渲染应是纯函数（同输入同输出）。
    // 视图在绘制过程中读全局时钟会引入不可复现的时间依赖，属反模式
    // （见 debug::g_paint_depth / Widget::paint 的 PaintPurityGuard）。
    AURORA_ASSERT(debug::g_paint_depth == 0,
                  "渲染纯度违例：在 Widget::paint() 中读取了全局时钟（current_timestamp）。"
                  "时间应通过 State/Signal 显式传入，而非在绘制时读可变全局。");
#endif
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace aurora
