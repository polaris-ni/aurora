#pragma once

/**
 * @file perf_log.h
 * @brief 性能日志导出：定期输出帧统计摘要，支持 JSON/CSV 快照。
 *
 * 数据源两路：
 * - `FrameStats`（滑动窗口帧时间，**不受 `AURORA_ENABLE_PROFILING` 影响，始终可用**）；
 * - `RenderCounters`（当帧确定性计数，仅在 `AURORA_ENABLE_PROFILING` 开启时非零）。
 *
 * 通道分工遵循项目硬规则第 8 条：
 * - `on_frame_end()` 的周期摘要属**诊断**，走 `AURORA_LOG_INFO`（stderr，可被级别过滤）；
 * - `dump_json()` / `dump_csv()` 属**程序产品**输出，走 `AURORA_LOG_RAW`（stdout，无前缀）。
 */

#include <string>

namespace aurora {

/// @brief 性能日志导出：定期输出 FrameStats + RenderCounters 摘要，支持 JSON/CSV 快照。
class PerfLog {
  public:
    /// @brief 启用定期日志输出（每 interval_frames 帧输出一次）。
    static auto enable(int interval_frames = 300) -> void;

    /// @brief 禁用定期日志输出。
    static auto disable() -> void;

    /// @brief 是否已启用。
    [[nodiscard]] static auto enabled() -> bool;

    /// @brief 帧结束时调用（由帧循环消费），每 interval_frames 帧自动输出日志。
    static auto on_frame_end() -> void;

    /**
     * @brief 生成 JSON 格式快照。
     *
     * 顶层为 `FrameStats` 指标，`counters` 子对象为当帧 `RenderCounters`，
     * `profiling` 标记本次构建是否开启细粒度插桩（关闭时 counters 恒为 0）。
     */
    [[nodiscard]] static auto snapshot_json() -> std::string;

    /// @brief 生成 CSV 格式快照（含表头注释行 + 单数据行，尾部为计数器列）。
    [[nodiscard]] static auto snapshot_csv() -> std::string;

    /// @brief CSV 表头（不含注释前缀，与 `snapshot_csv()` 的数据行严格对应）。
    [[nodiscard]] static auto csv_header() -> std::string;

    /// @brief 经 `AURORA_LOG_RAW` 输出 JSON 快照（带换行），供脚本消费。
    static auto dump_json() -> void;

    /// @brief 经 `AURORA_LOG_RAW` 输出 CSV 快照（带换行），供趋势表消费。
    static auto dump_csv() -> void;

  private:
    static auto log_summary() -> void;

    static inline bool s_enabled = false; // NOLINT(readability-identifier-naming)
    static inline int s_interval = 300;   // NOLINT(readability-identifier-naming)
    static inline int s_counter = 0;      // NOLINT(readability-identifier-naming)
};

} // namespace aurora
