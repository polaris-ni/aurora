#pragma once

/// @brief 作用域性能采集器与长任务归因。
/// @file profiler.h
///
/// 设计要点：
/// - **热路径零分配**：zone 名为静态字符串字面量（不拷贝、不拥有），样本存入
/// 构造期预留容量的环形缓冲；超出容量丢弃并计数，绝不在帧内触发堆分配。
/// - **绝不在热路径写日志**：`AURORA_LOG_*` 存在「级别被过滤但参数仍求值」的陷阱，
/// 本类只在 `report_*` 时批量产出字符串，且交由调用方经 `AURORA_LOG_RAW` 输出。
/// - **编译期归零**：`AURORA_ENABLE_PROFILING` 关闭时 `AURORA_PROFILE_SCOPE` 展开为空。
/// 类本身仍参与编译与链接，便于单测直接驱动。
///

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/perf/counters.h"
#include "aurora/perf/stopwatch.h"

namespace aurora {

/// @brief 单个作用域计时样本。
///
/// @warning `name` 为不拥有所有权的静态字符串指针（通常是字符串字面量或 `__func__`），
/// 生命周期须覆盖整个进程。禁止传入临时 `std::string` 的 `c_str()`。
///
struct ZoneSample {
    const char *name = nullptr;  ///< 静态字符串，不拥有所有权
    double start_ms = 0.0;  ///< 相对本帧起点的偏移（毫秒）
    double duration_ms = 0.0;  ///< 作用域耗时（毫秒）
    std::uint16_t depth = 0;  ///< 嵌套深度（0 = 顶层），用于火焰图与 trace 缩进
};

/// @brief 同名 zone 在一帧内的聚合结果。
struct ZoneAggregate {
    const char *name = nullptr;  ///< 静态字符串，不拥有所有权
    std::uint32_t call_count = 0;  ///< 调用次数
    double total_ms = 0.0;  ///< 总耗时
    double max_ms = 0.0;  ///< 单次最大耗时
};

/// @brief 作用域性能采集器（进程级单例）。
///
/// 帧协议：`begin_frame()` → 若干 `begin_zone()` / `end_zone()` 配对 → `end_frame()`。
/// 通常不直接调用 zone 接口，而是用 `AURORA_PROFILE_SCOPE` / `AURORA_PROFILE_FUNCTION`。
///
/// @note Thread: main-thread only（单线程 UI 模型，内部无锁）
/// @note Side-effects: none（不写日志、不写文件）
///
class Profiler {
  public:
    /// @brief 默认单帧 zone 样本容量（超出即丢弃并计入 `dropped_zones()`）。
    static constexpr std::size_t AURORA_DEFAULT_ZONE_CAPACITY = 512;  // NOLINT(readability-identifier-naming)

    /// @brief 最大嵌套深度（超出即丢弃并计入 `dropped_zones()`）。
    static constexpr std::size_t AURORA_MAX_ZONE_DEPTH = 64;  // NOLINT(readability-identifier-naming)

    /// @brief 默认长任务阈值（毫秒）= 60fps 帧预算的一半。
    static constexpr double AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS = 8.333333;  // NOLINT(readability-identifier-naming)

    /// @brief 取得全局唯一实例。
    /// @return 进程级单例的引用（首次调用时构造，存活至进程结束）。
    [[nodiscard]] static auto instance() -> Profiler &;

    // ---- 帧协议 ----

    /// @brief 帧起点：重置当帧 zone 列表与时间原点（不清空跨帧累计统计）。
    auto begin_frame() -> void;

    /// @brief 帧终点：结算未闭合 zone 的告警计数，推进帧序号。
    auto end_frame() -> void;

    /// @brief 当前帧序号（自 `reset()` 起累加）。
    /// @return 已闭合的帧数（每次 `end_frame()` 加一）。
    [[nodiscard]] auto frame_index() const -> std::uint64_t;

    /// @brief 当前帧起点的进程内时间戳（毫秒，见 `Stopwatch::now_ms()`）。
    /// @return 本帧原点毫秒时间戳，zone 的 `start_ms` 即相对它的偏移。
    [[nodiscard]] auto frame_start_ms() const -> double;

    // ---- 作用域协议 ----

    /// @brief 进入一个计时作用域。
    /// @param name 静态字符串字面量；生命周期须覆盖整个进程。
    ///
    auto begin_zone(const char *name) -> void;

    /// @brief 离开最近进入的计时作用域（与 `begin_zone` 严格配对）。
    auto end_zone() -> void;

    // ---- 查询 ----

    /// @brief 当帧已闭合的全部 zone 样本（按闭合顺序）。
    /// @return 样本列表的常引用；下次 `begin_frame()` 即清空，须在帧内消费或快照。
    [[nodiscard]] auto frame_zones() const -> const std::vector<ZoneSample> &;

    /// @brief 按名字聚合当帧样本；无匹配时返回 `call_count == 0` 的空结果。
    /// @param name 要匹配的 zone 名（按内容比较，允许来自不同翻译单元的同名字面量）。
    /// @return 该 zone 当帧的次数、总耗时与单次最大耗时；`agg.name` 即传入的 `name`。
    [[nodiscard]] auto aggregate(const char *name) const -> ZoneAggregate;

    /// @brief 当帧全部 zone 的聚合列表（按总耗时降序）。
    /// @return 新分配的聚合向量（每个不同 zone 名一条）；属冷路径，勿在帧内热路径调用。
    [[nodiscard]] auto aggregates() const -> std::vector<ZoneAggregate>;

    // ---- 长任务 ----

    /// @brief 设置长任务判定阈值（毫秒）。
    /// @param ms 新阈值；zone 耗时 >= ms 即计入长任务（比较在 `end_zone` 闭合时进行）。
    auto set_long_task_threshold_ms(double ms) -> void;

    /// @brief 当前长任务判定阈值（毫秒）。
    /// @return 生效中的长任务阈值。
    [[nodiscard]] auto long_task_threshold_ms() const -> double;

    /// @brief 当帧被判定为长任务的 zone 样本。
    /// @return 长任务样本列表的常引用（预留容量 64，超出仅累计 `total_long_task_count` 不存样本）。
    [[nodiscard]] auto long_tasks() const -> const std::vector<ZoneSample> &;

    /// @brief 自 `reset()` 起累计的长任务次数（跨帧）。
    /// @return 历帧闭合时长达标 zone 的总次数。
    [[nodiscard]] auto total_long_task_count() const -> std::uint64_t;

    // ---- 容量与诊断 ----

    /// @brief 设置单帧 zone 样本容量并预留内存。
    /// @param capacity 单帧样本槽数；传 0 会被归一为 1。
    /// @note 仅在帧外调用；会清空当帧已采集样本。
    ///
    auto set_zone_capacity(std::size_t capacity) -> void;

    /// @brief 当前单帧 zone 样本容量。
    /// @return 生效中的样本槽数（超出即丢弃并计入 `dropped_zones()`）。
    [[nodiscard]] auto zone_capacity() const -> std::size_t;

    /// @brief 因超出容量/深度而被丢弃的样本数（>0 说明容量需要调大）。
    /// @return 自 `reset()` 起累计的丢弃样本数。
    [[nodiscard]] auto dropped_zones() const -> std::uint64_t;

    /// @brief 检测到的 `begin_zone`/`end_zone` 配对错误次数。
    /// @return 自 `reset()` 起累计的失配次数（end 多于 begin，或开帧时仍有未闭合 zone）。
    [[nodiscard]] auto unbalanced_zones() const -> std::uint64_t;

    // ---- 开关与清理 ----

    /// @brief 运行时二级开关（在编译期宏已开启的前提下生效）。
    /// @param on true 恢复采集；false 令 `begin_zone`/`end_zone` 走空路径。
    /// @note 关闭后 `begin_zone`/`end_zone` 立即返回，不产生样本。
    ///
    auto set_enabled(bool on) -> void;

    /// @brief 运行时开关状态。
    /// @return 当前二级开关是否开启。
    [[nodiscard]] auto is_enabled() const -> bool;

    /// @brief 清空全部状态（帧序号、跨帧累计、当帧样本），保留容量与阈值配置。
    auto reset() -> void;

    /// @brief 生成当帧聚合的多行文本报告（不含尾随换行）。
    /// @return 表头 + 每行一个聚合 zone（名称、次数、总耗时、最大耗时）的字符串；无样本时仅表头。
    /// @note 仅在需要输出时调用；调用方负责经 `AURORA_LOG_RAW` 写出。
    ///
    [[nodiscard]] auto report_text() const -> std::string;

  private:
    Profiler();

    struct OpenZone {
        const char *name = nullptr;
        double start_ms = 0.0;  ///< 相对帧起点
        Stopwatch watch;  ///< 本 zone 的高精度计时器：begin_zone 时复位，闭合时折算毫秒
    };

    bool enabled_ = true;
    double long_task_threshold_ms_ = AURORA_DEFAULT_LONG_TASK_THRESHOLD_MS;
    std::size_t capacity_ = AURORA_DEFAULT_ZONE_CAPACITY;

    double frame_start_ms_ = 0.0;
    std::uint64_t frame_index_ = 0;

    std::vector<ZoneSample> zones_;  ///< 当帧已闭合样本（容量预留，帧内不再分配）
    std::vector<ZoneSample> long_tasks_;  ///< 当帧长任务
    OpenZone stack_[AURORA_MAX_ZONE_DEPTH]{};
    // 未闭合 zone 栈（固定容量，零分配）
    std::size_t depth_ = 0;

    std::uint64_t dropped_ = 0;
    std::uint64_t unbalanced_ = 0;
    std::uint64_t total_long_tasks_ = 0;
};

/// @brief RAII 作用域计时：构造进入 zone，析构离开 zone。
///
/// 通常经 `AURORA_PROFILE_SCOPE` 使用而非直接构造。
/// @note Thread: main-thread only
///
class ScopedTimer {
  public:
    /// @brief 构造并进入计时作用域（等价于 `Profiler::begin_zone(name)`）。
    /// @param name 静态字符串字面量；生命周期须覆盖整个进程。
    explicit ScopedTimer(const char *name) { Profiler::instance().begin_zone(name); }

    /// @brief 析构并闭合作用域（等价于 `Profiler::end_zone()`）；内部吞掉全部异常，保证析构不抛出。
    ~ScopedTimer() {
        try {
            Profiler::instance().end_zone();
        } catch (...) {  // NOLINT(*-empty-catch)
            // 析构不得抛出：性能采集失败绝不拖垮应用
        }
    }

    ScopedTimer(const ScopedTimer &) = delete;
    auto operator=(const ScopedTimer &) -> ScopedTimer & = delete;
    ScopedTimer(ScopedTimer &&) = delete;
    auto operator=(ScopedTimer &&) -> ScopedTimer & = delete;
};

namespace detail {

/// @brief 帧作用域收尾钩子：把当帧 zone 样本与计数快照转交 `TraceWriter`。
///
/// 定义在 `profiler.cpp`，避免 `profiler.h` 反向依赖 `trace_writer.h`（后者依赖前者）。
/// `AURORA_ENABLE_TRACING` 未开启时为空实现（每帧一次空调用，开销可忽略）。
/// @internal 仅供 `FrameScope` 调用，不属于公共 API。
///
auto on_frame_scope_end() -> void;

}  // namespace detail

/// @brief RAII 帧作用域：构造开帧（并清零当帧渲染计数器），析构闭帧。
///
/// 与 `ScopedTimer` 的区别：帧作用域同时管理 `Profiler` 的帧协议与 `RenderCounters`
/// 的每帧归零，使「当帧 zone 样本」与「当帧计数」严格同源、同生命周期。
///
/// 析构后数据仍可读（`begin_frame` 才会清空），故 `present_root` 返回后
/// 采集器（`ScrollBenchHarness` / `PerfSession`）可安全快照当帧结果。
///
/// @note Thread: main-thread only
///
class FrameScope {
  public:
    /// @brief 开帧：调用 `Profiler::begin_frame()` 并把当帧渲染计数器清零，使样本与计数同源。
    FrameScope() {
        Profiler::instance().begin_frame();
        RenderCounters::current().reset();
    }

    /// @brief 闭帧：先把当帧快照喂给 `TraceWriter`，再调用 `Profiler::end_frame()`。
    /// @note 顺序不可颠倒：`end_frame()` 会推进帧序号，颠倒将使事件挂到下一帧。
    ~FrameScope() {
        try {
            detail::on_frame_scope_end();
            Profiler::instance().end_frame();
        } catch (...) {  // NOLINT(*-empty-catch)
            // 析构不得抛出：性能采集失败绝不拖垮应用
        }
    }

    FrameScope(const FrameScope &) = delete;
    auto operator=(const FrameScope &) -> FrameScope & = delete;
    FrameScope(FrameScope &&) = delete;
    auto operator=(FrameScope &&) -> FrameScope & = delete;
};

}  // namespace aurora

// ---------------------------------------------------------------------------
// 作用域埋点宏
//
// `AURORA_ENABLE_PROFILING` 关闭时完全展开为 `((void)0)`，不产生任何指令，
// 也不实例化 `ScopedTimer`。
// ---------------------------------------------------------------------------

/// @brief 内部辅助宏：两级拼接，令 `__LINE__` 等实参先展开再参与连接。
#define AURORA_PROF_CAT_IMPL(a, b) a##b

/// @brief 记号连接宏：为当前作用域生成唯一命名的计时变量。
#define AURORA_PROF_CAT(a, b) AURORA_PROF_CAT_IMPL(a, b)  // NOLINT(cppcoreguidelines-macro-usage)

#ifdef AURORA_ENABLE_PROFILING
/// @brief 为当前作用域计时；`name` 必须是静态字符串字面量。
#define AURORA_PROFILE_SCOPE(name) \
    ::aurora::ScopedTimer AURORA_PROF_CAT(_au_zone_, __LINE__) { name }
/// @brief 为当前函数计时（zone 名取 `__func__`）。
#define AURORA_PROFILE_FUNCTION() AURORA_PROFILE_SCOPE(__func__)
/// @brief 在当前作用域开启一帧（开帧清零计数器，离开作用域闭帧）。
#define AURORA_PROFILE_FRAME() \
    ::aurora::FrameScope AURORA_PROF_CAT(_au_frame_, __LINE__) {}
#else
/// @brief 性能采集关闭时展开为 `((void)0)`：不计时，`name` 实参也不被求值。
#define AURORA_PROFILE_SCOPE(name) ((void)0)  // NOLINT(cppcoreguidelines-macro-usage)
/// @brief 性能采集关闭时展开为 `((void)0)`：不计时。
#define AURORA_PROFILE_FUNCTION() ((void)0)  // NOLINT(cppcoreguidelines-macro-usage)
/// @brief 性能采集关闭时展开为 `((void)0)`：不开帧也不闭帧。
#define AURORA_PROFILE_FRAME() ((void)0)  // NOLINT(cppcoreguidelines-macro-usage)
#endif
