#pragma once

/**
 * @file perf_session.h
 * @brief 一次测量会话的聚合与报告。
 *
 * `FrameStats` 是**滑动窗口**（固定 128 帧，用于屏幕叠加实时观察），天然不适合
 * 「跑 300 帧然后给结论」的基准场景：窗口会滚掉早期帧，且没有计数器与 zone 维度。
 * `PerfSession` 补上这一层——**全量留存**会话内每帧的耗时与计数，会话结束一次性
 * 算出 p50/p95/p99、抖动、长任务数、整帧重绘帧数与计数器汇总/峰值。
 *
 * 验收看 `p99_ms` / `jitter_ms` / `full_redraw_frames` 三项，而非 `avg_frame_ms`
 * ——均值会把偶发的 80ms 卡顿摊平成看不见。
 *
 * 输出三种形态：`to_markdown()` 给人读、`to_json()` 给脚本读、`to_csv_row()` 给趋势表。
 * 全部由调用方经 `AURORA_LOG_RAW` 输出（项目硬规则第 8 条）。
 */

#include <cstddef>
#include <string>
#include <vector>

#include "aurora/perf/counters.h"
#include "aurora/perf/profiler.h"

namespace aurora {

/**
 * @brief 一次测量会话的聚合结果（纯数据，可自由拷贝与比较）。
 *
 * @note Thread: any（不可变值语义）
 * @note Side-effects: none
 */
struct PerfReport {
    std::string name;  ///< 会话名（写入报告标题与 JSON/CSV）
    std::size_t frame_count = 0;  ///< 采样帧数（不含 warmup）
    double total_ms = 0.0;  ///< 采样帧耗时总和

    // ---- 帧时间分布（毫秒）----
    double avg_frame_ms = 0.0;  ///< 均值：仅作参考，不作验收依据
    double p50_ms = 0.0;  ///< 中位数
    double p95_ms = 0.0;  ///< 95 分位
    double p99_ms = 0.0;  ///< 99 分位：**主验收指标**
    double best_ms = 0.0;  ///< 最快帧
    double worst_ms = 0.0;  ///< 最慢帧
    double jitter_ms = 0.0;  ///< 标准差：**主验收指标**（手感平顺度）

    // ---- 关键计次 ----
    double frame_budget_ms = 16.67;  ///< 帧预算（默认 60fps）
    std::size_t over_budget_frames = 0;  ///< 超预算帧数
    std::size_t long_task_count = 0;  ///< 长任务累计次数（Profiler 判定）
    std::size_t full_redraw_frames = 0;  ///< **主验收指标**：退化为整帧重绘的帧数

    // ---- 渲染计数 ----
    RenderCounters counters_sum{};  ///< 会话内逐字段累加（`dirty_area_ratio` 为和，需自行除帧数）
    RenderCounters counters_max{};  ///< 会话内逐字段峰值

    /// @brief 会话级 zone 聚合（按总耗时降序；`AURORA_ENABLE_PROFILING` 关闭时为空）。
    std::vector<ZoneAggregate> zones;

    /// @brief 超预算帧占比 [0,1]。
    [[nodiscard]] auto over_budget_ratio() const -> double;

    /// @brief 脏区面积占比均值 [0,1]（`counters_sum.dirty_area_ratio / frame_count`）。
    [[nodiscard]] auto avg_dirty_area_ratio() const -> double;

    /// @brief 序列化为 JSON 对象（含 counters 子对象与 zones 数组）。
    [[nodiscard]] auto to_json() const -> std::string;

    /// @brief 渲染为 Markdown 报告（指标表 + zone 表），复用 `bench_render` 的表格风格。
    [[nodiscard]] auto to_markdown() const -> std::string;

    /// @brief 序列化为 CSV 数据行（字段顺序与 `csv_header()` 一致）。
    [[nodiscard]] auto to_csv_row() const -> std::string;

    /// @brief CSV 表头（与 `to_csv_row()` 严格对应，含 `RenderCounters::csv_header()` 后缀）。
    [[nodiscard]] static auto csv_header() -> std::string;
};

/**
 * @brief 测量会话：逐帧收集耗时 + 计数 + zone，会话结束产出 `PerfReport`。
 *
 * 与 `Profiler`（单例、当帧粒度）分工：`Profiler` 管**帧内**，`PerfSession` 管**跨帧**。
 *
 * @code
 *   PerfSession sess{ "scroll-300", 300 };
 *   for (int i = 0; i < 300; ++i) {
 *       Stopwatch sw;
 *       { AURORA_PROFILE_FRAME(); render_one_frame(); }   // 帧内 zone + 计数
 *       sess.record_frame(sw.elapsed_ms());               // 帧外快照
 *   }
 *   AURORA_LOG_RAW("perf", sess.report().to_markdown(), "\n");
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none（不写日志、不写文件）
 */
class PerfSession {
  public:
    /// @brief 默认预留帧数（避免采样期间扩容影响读数）。
    static constexpr std::size_t AURORA_DEFAULT_RESERVE_FRAMES = 512;  // NOLINT(readability-identifier-naming)

    /**
     * @brief 构造会话并预留容量。
     * @param name 会话名，写入报告标题
     * @param reserve_frames 预留帧数；实际帧数超出会扩容（仅影响读数精度，不影响正确性）
     */
    explicit PerfSession(std::string name = "session", std::size_t reserve_frames = AURORA_DEFAULT_RESERVE_FRAMES);

    /// @brief 清空已采样数据并重新预留（会话可复用）。
    auto begin(std::size_t reserve_frames = 0) -> void;

    /**
     * @brief 记录一帧：耗时 + 当前 `RenderCounters` + `Profiler` 当帧 zone / 长任务。
     * @param frame_ms 整帧耗时（毫秒），由调用方用 `Stopwatch` 测得
     * @note 须在帧作用域**结束之后**调用，否则计数与 zone 尚未定型。
     */
    auto record_frame(double frame_ms) -> void;

    /**
     * @brief 记录一帧（显式给定计数快照，不读取全局单例）。
     * @param frame_ms 整帧耗时（毫秒）
     * @param counters 当帧计数快照
     * @note 供单测构造确定性数据，避免依赖进程级状态。
     */
    auto record_frame(double frame_ms, const RenderCounters &counters) -> void;

    /// @brief 已采样帧数。
    [[nodiscard]] auto frame_count() const -> std::size_t;

    /// @brief 只读访问逐帧耗时（毫秒，按记录顺序）。
    [[nodiscard]] auto frame_times_ms() const -> const std::vector<double> &;

    /// @brief 设置帧预算（毫秒），影响 `over_budget_frames`。
    auto set_frame_budget_ms(double ms) -> void;

    /// @brief 当前帧预算（毫秒）。
    [[nodiscard]] auto frame_budget_ms() const -> double;

    /// @brief 会话名。
    [[nodiscard]] auto name() const -> const std::string &;

    /// @brief 结算并产出报告（不修改会话状态，可重复调用）。
    [[nodiscard]] auto report() const -> PerfReport;

    /// @brief 清空全部采样数据（保留会话名与帧预算）。
    auto clear() -> void;

  private:
    /// @brief 把一帧的 zone 聚合并入会话级聚合（按名字内容匹配）。
    auto merge_zones(const std::vector<ZoneAggregate> &frame_zones) -> void;

    std::string name_;
    double frame_budget_ms_ = 16.67;

    std::vector<double> frame_ms_;
    std::vector<ZoneAggregate> zones_;

    RenderCounters sum_{};
    RenderCounters max_{};
    std::size_t long_tasks_ = 0;
    std::size_t full_redraw_frames_ = 0;
};

}  // namespace aurora
