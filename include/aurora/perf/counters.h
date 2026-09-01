#pragma once

/**
 * @file counters.h
 * @brief 确定性渲染计数器。
 *
 * 与时间指标的根本区别：**计数在 Headless 下完全确定**，不随机器负载、编译器、
 * 后台进程漂移，因此可以像 golden 图那样锁基线并写入 CI 断言。
 * 「滚动一格后 `full_redraw == true`」这类断言稳定可靠；`frame_ms < X` 在共享
 * CI 机器上必然 flaky。
 */

#include <cstdint>
#include <string>

namespace aurora {

/**
 * @brief 细粒度性能埋点是否在本次构建中启用。
 *
 * 由 CMake 开关 `AURORA_ENABLE_PROFILING` 决定（默认随构建类型：
 * Debug / RelWithDebInfo = ON，Release / MinSizeRel = OFF）。
 * 供测试与运行期代码在编译期分支，避免 `#ifdef` 扩散到调用点。
 */
[[nodiscard]] constexpr auto profiling_enabled() noexcept -> bool {
#ifdef AURORA_ENABLE_PROFILING
    return true;
#else
    return false;
#endif
}

/**
 * @brief 单帧渲染计数快照。
 *
 * 由绘制 / 布局热路径经 `AURORA_PROFILE_COUNT` / `AURORA_PROFILE_SET` 累加，
 * 帧循环在帧起点调用 `current().reset()`。所有字段在同一输入下**逐帧可复现**。
 *
 * 字段分四组：绘制原语、缓存效率、树遍历规模、脏区效率。
 * 其中 `layout_nodes` / `dl_records` / `full_redraw` / `dirty_area_ratio`
 * 四项是回归防线的核心。
 *
 * @note Thread: main-thread only（单线程 UI 模型，无锁）
 * @note Side-effects: none
 */
struct RenderCounters {
    // ---- 绘制原语 ----
    std::uint32_t draw_calls = 0;      ///< DisplayList 命令总数（录制 + 直绘）
    std::uint32_t fill_rects = 0;      ///< 矩形填充次数（含圆角/渐变变体）
    std::uint32_t draw_texts = 0;      ///< 文本绘制调用次数
    std::uint32_t glyphs_rendered = 0; ///< 实际光栅化/贴出的字形数
    std::uint64_t pixels_filled = 0;   ///< 填充率：实际写入帧缓冲的像素数

    // ---- 缓存效率 ----
    std::uint32_t glyph_cache_hits = 0;   ///< 字形位图 atlas 命中
    std::uint32_t glyph_cache_misses = 0; ///< 字形位图 atlas 未命中（触发光栅化）
    std::uint32_t shape_cache_hits = 0;   ///< 文本 shaping 缓存命中
    std::uint32_t shape_cache_misses = 0; ///< 文本 shaping 缓存未命中（触发 hb_shape）
    std::uint32_t dl_replays = 0;         ///< DisplayList 回放次数（越高越好）
    std::uint32_t dl_records = 0;         ///< DisplayList 重录次数（越低越好）

    // ---- 树遍历规模 ----
    std::uint32_t layout_nodes = 0;            ///< 本帧实际执行 on_layout 的节点数
    std::uint32_t paint_nodes = 0;             ///< 本帧实际执行 on_paint 的节点数
    std::uint32_t relayout_boundaries_hit = 0; ///< 布局脏在边界处被截断的次数

    // ---- 脏区效率 ----
    std::uint32_t dirty_rect_count = 0; ///< 本帧脏矩形数量
    double dirty_area_ratio = 0.0;      ///< 脏区面积 / 全屏面积，取值 [0,1]
    bool full_redraw = false;           ///< 本帧是否退化为整帧重绘

    // ---- 内存占用 ----
    std::uint64_t scroll_buffer_bytes = 0; ///< Scroll 离屏缓冲字节数

    /**
     * @brief 取当前帧的计数器（进程级单例）。
     *
     * 埋点宏经此访问；即便 `AURORA_ENABLE_PROFILING` 关闭本函数依然可用，
     * 便于测试直接构造与断言。
     */
    [[nodiscard]] static auto current() -> RenderCounters &;

    /// @brief 全部字段归零（帧起点调用）。
    auto reset() -> void;

    /**
     * @brief 逐字段累加另一份计数（用于跨帧汇总）。
     *
     * `full_redraw` 按逻辑或合并；`dirty_area_ratio` 按算术和累加
     * （调用方自行除以帧数得到均值）。
     */
    auto add(const RenderCounters &other) -> void;

    /// @brief 逐字段取最大值合并（用于统计跨帧峰值）。
    auto merge_max(const RenderCounters &other) -> void;

    /// @brief 序列化为单行 JSON 对象（供 PerfLog / 报告消费）。
    [[nodiscard]] auto to_json() const -> std::string;

    /// @brief 序列化为 CSV 数据行（字段顺序与 `csv_header()` 一致）。
    [[nodiscard]] auto to_csv_row() const -> std::string;

    /// @brief CSV 表头（与 `to_csv_row()` 严格对应）。
    [[nodiscard]] static auto csv_header() -> std::string_view;
};

} // namespace aurora

// ---------------------------------------------------------------------------
// 计数埋点宏
//
// `AURORA_ENABLE_PROFILING` 关闭时完全展开为 `((void)0)`，不产生任何指令。
// ⚠️ 约束：宏参数**不得带副作用**（关闭时不会被求值）。若某个计数值的计算本身
// 有代价或有副作用，调用点须自行用 `#ifdef AURORA_ENABLE_PROFILING` 包裹。
// ---------------------------------------------------------------------------
#ifdef AURORA_ENABLE_PROFILING
/// @brief 累加计数字段：`AURORA_PROFILE_COUNT(fill_rects, 1)`。
#define AURORA_PROFILE_COUNT(field, n) (void)(::aurora::RenderCounters::current().field += (n))
/// @brief 直接赋值计数字段：`AURORA_PROFILE_SET(full_redraw, true)`。
#define AURORA_PROFILE_SET(field, v) (void)(::aurora::RenderCounters::current().field = (v))
#else
#define AURORA_PROFILE_COUNT(field, n) ((void)0) // NOLINT(cppcoreguidelines-macro-usage)
#define AURORA_PROFILE_SET(field, v) ((void)0)   // NOLINT(cppcoreguidelines-macro-usage)
#endif
