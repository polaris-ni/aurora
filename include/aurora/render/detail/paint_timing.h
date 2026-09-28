#pragma once

/// @brief 光栅热点耗时累加器与 RAII 计时器：paint/scene 各阶段的帧内计时与计数，供 DEBUG 性能排查。
/// @file

#include <chrono>

namespace aurora::detail {

/// @brief 各光栅热点耗时累加器（仅 DEBUG 排查用，开销可忽略）。
/// 字段覆盖：直接绘制原语（fill/text/shadow/gradient/image/blur/border/line）、离屏合成（composite）、
/// 帧缓冲维护（clear/shift）、就地后处理（region）、整段 widget 绘制（scene）、其遍历/调度胶水（glue）
/// 与其余（other）。仅供 DEBUG 排查，不用于生产。
struct PaintTiming {
    double fill = 0;  ///< fillRect/矩形类原语耗时累加（毫秒）。
    double text = 0;  ///< 文本栅格耗时累加（毫秒）。
    double shadow = 0;  ///< 阴影绘制耗时累加（毫秒）。
    double gradient = 0;  ///< 渐变绘制耗时累加（毫秒）。
    double image = 0;  ///< 图像绘制耗时累加（毫秒）。
    double blur = 0;  ///< 模糊处理耗时累加（毫秒）。
    double border = 0;  ///< draw_rounded_border 耗时累加（毫秒）。
    double line = 0;  ///< draw_line 耗时累加（毫秒）。
    double composite = 0, clear = 0, shift = 0,
           region = 0;  ///< 离屏合成 / 帧缓冲 clear / 平移 shift / 就地后处理 region 耗时累加（毫秒）。
    double scene = 0, glue = 0,
           other =
               0;  ///< 整段 widget 绘制耗时、scene 内非栅格的「树遍历/DL 回放/调度」胶水耗时与其余耗时累加（毫秒）。
    std::uint64_t paint_nodes = 0;  ///< 本帧真正参与 paint 遍历的节点数（DL 命中不计）。
    std::uint64_t dl_replays = 0;  ///< 本帧 DisplayList 命中回放次数。
    std::uint64_t dl_records = 0;  ///< 本帧 DisplayList 录制次数（含未命中缓存的首录）。
    std::uint64_t scroll_r_whole = 0;  ///< 累积：Scroll 整块重录帧数（缓冲无效/布局标脏）。
    std::uint64_t scroll_r_band = 0;  ///< 累积：Scroll 局部（脏带）重录帧数（动画后代标脏）。
    std::uint64_t scroll_r_reanchor = 0;  ///< 累积：Scroll 增量重锚帧数（长内容滚动）。
    std::uint64_t scroll_r_blit = 0;  ///< 累积：Scroll 仅平移合成帧数（理想稳态应≈绘制帧数）。
};

/// @brief RAII 计时器：构造记录起点，析构把耗时累加到 *cat。
/// @param c 指向目标耗时累加单元（如 PaintTiming 的某字段）。
/// @param act false 时析构不计时，用于避免嵌套 record 重复累加。
struct PaintTimer {
    double *cat;  ///< 析构时把本次耗时累加到的目标单元。
    bool active;  ///< 是否启用计时；false 时析构直接返回。
    std::chrono::steady_clock::time_point t0;  ///< 构造时记录的起点时刻。
    /// @brief 构造并记录起点时刻。
    /// @param c 指向目标耗时累加单元。
    /// @param act false 时析构不累加，避免嵌套 record 重复计时。
    explicit PaintTimer(double *c, bool act = true) : cat(c), active(act), t0(std::chrono::steady_clock::now()) {}
    /// @brief 析构时把起点至今的毫秒数累加到 *cat（active=false 时跳过）。
    ~PaintTimer() {
        if (!active) {
            return;
        }
        const auto t1 = std::chrono::steady_clock::now();
        *cat += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    // RAII 栈上计时器：禁止复制/移动，避免对同一 *cat 重复累加或悬空写入。
    PaintTimer(const PaintTimer &) = delete;
    auto operator=(const PaintTimer &) -> PaintTimer & = delete;
    PaintTimer(PaintTimer &&) = delete;
    auto operator=(PaintTimer &&) -> PaintTimer & = delete;
};

/// @brief 整段 widget 绘制计时（scene）：仅累加耗时，不触碰 g_raster_depth（避免把「树遍历」误判为栅格内）。
struct SceneTimer {
    double *cat;  ///< 析构时把本次耗时累加到的目标单元。
    std::chrono::steady_clock::time_point t0;  ///< 构造时记录的起点时刻。
    /// @brief 构造并记录起点时刻。
    /// @param c 指向目标耗时累加单元。
    explicit SceneTimer(double *c) : cat(c), t0(std::chrono::steady_clock::now()) {}
    /// @brief 析构时把起点至今的毫秒数累加到 *cat。
    ~SceneTimer() {
        const auto t1 = std::chrono::steady_clock::now();
        *cat += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    // RAII 栈上计时器：禁止复制/移动，避免对同一 *cat 重复累加或悬空写入。
    SceneTimer(const SceneTimer &) = delete;
    auto operator=(const SceneTimer &) -> SceneTimer & = delete;
    SceneTimer(SceneTimer &&) = delete;
    auto operator=(SceneTimer &&) -> SceneTimer & = delete;
};

/// @brief 返回当前帧累加器引用（定义在 painter.cpp），供各热点就地计时/计数。
/// @return 当前帧的 PaintTiming 累加器。
auto paint_timing() -> PaintTiming &;
/// @brief 返回上一完成绘制帧的快照引用（由 commit_paint_frame 写入，不随帧累加）。
/// @return 上一绘制帧的 PaintTiming 快照。
auto paint_timing_last() -> const PaintTiming &;
/// @brief 提交一帧：把当前帧累加器快照为「上一帧」并清零。
/// drew=false（idle 帧）时不覆盖快照，但仍清零当前帧（idle 帧不进 paint 域，累加器本就为空，清零下帧从零计）。
/// @param drew 本帧是否发生过绘制；false 表示 idle 帧，仅清零不覆盖快照。
auto commit_paint_frame(bool drew) -> void;

}  // namespace aurora::detail
