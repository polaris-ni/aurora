#pragma once

/**
 * @file trace_writer.h
 * @brief Chrome Trace Event 格式输出。
 *
 * 产出可被 `chrome://tracing` / [Perfetto UI](https://ui.perfetto.dev) 直接打开的
 * JSON，用于**长任务归因**与**火焰图**——这是 `Profiler::report_text()` 的文本聚合
 * 给不了的信息：谁嵌套在谁里面、卡顿发生在帧内哪一段。
 *
 * 设计要点：
 * - **采集与序列化分离**：`capture_frame()` 只做 POD 拷贝（无字符串、无堆分配，
 *   容量预留后帧内零分配）；JSON 字符串仅在 `to_json()` / `write_json()` 时生成。
 * - **有界内存**：事件数达 `capacity()` 后丢弃并累加 `dropped_events()`，
 *   绝不因长时间录制把内存吃光。
 * - **静态字符串**：事件名沿用 `ZoneSample::name` 的静态字符串约定，不拷贝、不拥有。
 * - **输出走 `AURORA_LOG_RAW`**：`dump_to_log()` 属「程序产品」输出（项目硬规则第 8 条）。
 *
 * 时间轴：所有 `ts` / `dur` 以**微秒**写出（Trace Event 规范单位），
 * 数值来自 `Stopwatch::now_ms()` 的进程内单调时间原点。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/perf/counters.h"
#include "aurora/perf/profiler.h"

namespace aurora {

/**
 * @brief 追踪是否在本次构建中启用。
 *
 * 由 CMake 开关 `AURORA_ENABLE_TRACING` 决定（默认 OFF，开启时隐含开启
 * `AURORA_ENABLE_PROFILING`）。关闭时 `TraceWriter` 仍可被显式驱动（便于单测断言
 * 格式），只是 `FrameScope` 不再自动喂数据。
 */
[[nodiscard]] constexpr auto tracing_enabled() noexcept -> bool {
#ifdef AURORA_ENABLE_TRACING
    return true;
#else
    return false;
#endif
}

/// @brief Trace Event 相位（对应规范的 `ph` 字段，只用到本项目需要的三种）。
enum class TracePhase : std::uint8_t {
    Complete, ///< `"X"`：带时长的完整事件（zone），构成火焰图的一根柱子
    Instant,  ///< `"i"`：瞬时标记（帧边界、长任务点）
};

/**
 * @brief 单条追踪事件（POD，热路径按值拷贝）。
 *
 * @warning `name` 为不拥有所有权的静态字符串指针，生命周期须覆盖整个进程。
 */
struct TraceEvent {
    const char *name = nullptr;              ///< 静态字符串，不拥有所有权
    double ts_ms = 0.0;                      ///< 绝对时刻（`Stopwatch::now_ms()` 口径）
    double dur_ms = 0.0;                     ///< 时长；`Instant` 相位下无意义
    std::uint64_t frame_index = 0;           ///< 所属帧序号（写入 `args.frame`）
    std::uint16_t depth = 0;                 ///< 嵌套深度（写入 `args.depth`）
    TracePhase phase = TracePhase::Complete; ///< 事件相位
    bool long_task = false;                  ///< 是否被判定为长任务（写入 `args.long_task`）
};

/// @brief 计数器采样点：一帧的 `RenderCounters` 快照（写为 `ph:"C"` 计数事件）。
struct TraceCounterSample {
    double ts_ms = 0.0;            ///< 绝对时刻
    std::uint64_t frame_index = 0; ///< 帧序号
    RenderCounters counters{};     ///< 当帧计数快照
};

/**
 * @brief Chrome Trace Event 录制器（进程级单例）。
 *
 * 典型用法（帧循环外开关，帧内自动采集）：
 * @code
 *   TraceWriter::instance().begin_capture();
 *   run_frames(300);                       // FrameScope 每帧自动喂数据（需 TRACING=ON）
 *   TraceWriter::instance().end_capture();
 *   (void)TraceWriter::instance().write_json("scroll.trace.json");
 * @endcode
 *
 * @note Thread: main-thread only（单线程 UI 模型，内部无锁）
 * @note Side-effects: `write_json` 写文件；`dump_to_log` 写日志。其余接口无副作用
 */
class TraceWriter {
  public:
    /// @brief 默认事件容量（约覆盖 300 帧 × 200 zone；超出即丢弃并计数）。
    static constexpr std::size_t AURORA_DEFAULT_CAPACITY = 65536; // NOLINT(readability-identifier-naming)

    /// @brief 默认计数器采样容量（约 4096 帧）。
    static constexpr std::size_t AURORA_DEFAULT_COUNTER_CAPACITY = 4096; // NOLINT(readability-identifier-naming)

    /// @brief 取得全局唯一实例。
    [[nodiscard]] static auto instance() -> TraceWriter &;

    // ---- 录制开关 ----

    /// @brief 开始录制：清空既有事件并预留容量。
    auto begin_capture() -> void;

    /// @brief 结束录制：停止接收事件，已录数据保留待导出。
    auto end_capture() -> void;

    /// @brief 是否正在录制。
    [[nodiscard]] auto capturing() const -> bool;

    // ---- 采集 ----

    /**
     * @brief 抓取一帧：把 `Profiler` 当帧 zone 样本转为 Complete 事件。
     * @param prof 数据源；须在 `end_frame()` 之后、下一次 `begin_frame()` 之前调用。
     * @note 未在录制中时立即返回；热路径无字符串操作与堆分配（容量已预留）。
     */
    auto capture_frame(const Profiler &prof) -> void;

    /// @brief 抓取一帧的渲染计数快照（写为 Trace 计数器轨道）。
    auto capture_counters(std::uint64_t frame_index, double ts_ms, const RenderCounters &counters) -> void;

    /**
     * @brief 追加一条完整事件。
     * @param name        静态字符串字面量；生命周期须覆盖整个进程。
     * @param ts_ms       绝对时刻（`Stopwatch::now_ms()` 口径，微秒）。
     * @param dur_ms      事件时长（微秒）。
     * @param depth       嵌套深度（用于火焰图 Y 轴排列）。
     * @param frame_index 所属帧序号（写入 `args.frame`）。
     */
    auto add_complete_event(const char *name, double ts_ms, double dur_ms, std::uint16_t depth,
                            std::uint64_t frame_index) -> void;

    /// @brief 追加一条瞬时标记事件（如帧边界）。
    auto add_instant_event(const char *name, double ts_ms, std::uint64_t frame_index) -> void;

    // ---- 查询与容量 ----

    /// @brief 已录事件条数（不含计数器采样）。
    [[nodiscard]] auto event_count() const -> std::size_t;

    /// @brief 已录计数器采样点数。
    [[nodiscard]] auto counter_sample_count() const -> std::size_t;

    /// @brief 因超出容量被丢弃的事件数（>0 说明需要调大容量或缩短录制）。
    [[nodiscard]] auto dropped_events() const -> std::uint64_t;

    /// @brief 只读访问已录事件（供测试与自定义导出）。
    [[nodiscard]] auto events() const -> const std::vector<TraceEvent> &;

    /// @brief 只读访问计数器采样（供测试与自定义导出）。
    [[nodiscard]] auto counter_samples() const -> const std::vector<TraceCounterSample> &;

    /// @brief 设置事件容量并预留内存（仅在非录制期调用；会清空已录数据）。
    auto set_capacity(std::size_t events, std::size_t counter_samples = AURORA_DEFAULT_COUNTER_CAPACITY) -> void;

    /// @brief 当前事件容量。
    [[nodiscard]] auto capacity() const -> std::size_t;

    /// @brief 清空全部已录数据与丢弃计数（保留容量配置与录制状态）。
    auto clear() -> void;

    // ---- 导出 ----

    /**
     * @brief 序列化为 Chrome Trace Event JSON（数组格式，可直接打开）。
     *
     * 结构：进程/线程元数据事件 → zone 完整事件 → 帧计数器事件。
     * 时间戳换算为微秒；事件名做最小 JSON 转义。
     */
    [[nodiscard]] auto to_json() const -> std::string;

    /**
     * @brief 写入 JSON 文件。
     * @param path 目标路径（通常以 `.trace.json` 结尾）。
     * @return 成功返回 `true`；打开/写入失败返回 `IOFileNotFound` 错误。
     */
    [[nodiscard]] auto write_json(const char *path) const -> Result<bool>;

    /// @brief 经 `AURORA_LOG_RAW` 输出 JSON（供无文件系统的场景，如 Wasm）。
    auto dump_to_log() const -> void;

  private:
    TraceWriter();

    bool m_capturing = false;
    std::size_t m_capacity = AURORA_DEFAULT_CAPACITY;
    std::size_t m_counter_capacity = AURORA_DEFAULT_COUNTER_CAPACITY;
    std::uint64_t m_dropped = 0;

    std::vector<TraceEvent> m_events;
    std::vector<TraceCounterSample> m_counters;
};

} // namespace aurora
