#pragma once

/// @brief 面向下游应用的性能监控订阅门面：帧完成回调、命名指标、作用域计时与结构化快照。
/// @file frame_monitor.h
///
/// 与细粒度插桩的边界：本门面在 Release 构建下恒可用，不依赖 `AURORA_ENABLE_PROFILING`
/// （该开关默认取 AUTO 且不随安装产物导出）。零订阅者时门面不挂接底层采集、不分配、
/// 不读时钟、不写日志与文件；帧末入口只保留一次订阅数判空后立即返回。深度作用域插桩
/// 仍由既有编译期宏承担，本门面的作用域计时是下游自行命名的恒开入口。
///
/// 线程模型：单线程界面线程，帧末批量投递与订阅管理都在该线程内完成，无锁。
/// 快照语义：投递前取订阅者列表快照，回调期间注销只对下一批生效；回调捕获对象的
/// 生命周期由订阅者自行负责，注销后不得再回访已释放的捕获状态。
///

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace aurora {

/// @brief 单帧读数，是帧末批量投递的元素。
struct FrameSample {
    double frame_delta = 0.0;  ///< 帧循环传入的间隔，与 `FrameStats::record` 同一口径（秒）
    std::uint64_t index = 0;  ///< 进程内单调递增的帧序号
    bool idle = false;  ///< 是否为无实际渲染的 idle 帧
};

/// @brief 一次帧末投递的只读批量视图。
///
/// 生命周期仅限回调期间，回调返回后不得保留 `samples` 指针。
///
struct FrameBatch {
    const FrameSample *samples = nullptr;  ///< 批量首元素；`count` 为零时为空指针
    std::size_t count = 0;  ///< 批量内读数条数，上限见 `FrameMonitor::max_batch_frames()`
};

/// @brief 命名指标注册的最近一次结果。
enum class MetricError : std::uint8_t {
    None = 0,  ///< 尚无注册记录或最近一次注册成功
    DuplicateName = 1,  ///< 名称已被占用，既有指标未被覆盖
    InvalidName = 2,  ///< 名称为空或含控制字符
};

/// @brief 门面运行期诊断计数，用于零开销判据与背压观测。
///
/// 四项采集计数在零订阅者时恒为零：`hook_registrations`、`heap_allocations`、
/// `clock_reads` 与 `bytes_emitted`。订阅与命名指标注册这类显式管理动作不计入
/// 帧投递路径的分配计数；主动调用快照导出也不计入产物字节数。
///
struct MonitorDiagnostics {
    std::uint64_t hook_registrations = 0;  ///< 当前挂接的底层采集点数，无订阅者时为零
    std::uint64_t heap_allocations = 0;  ///< 帧投递路径发生的堆分配次数
    std::uint64_t clock_reads = 0;  ///< 门面读取单调钟的次数
    std::uint64_t bytes_emitted = 0;  ///< 帧投递路径产出的字节数
    std::uint64_t dropped_batches = 0;  ///< 因背压丢弃的批量数
    std::uint64_t subscriber_count = 0;  ///< 当前订阅者数
};

/// @brief 进程级性能监控门面，全部入口为静态函数，状态唯一且位于实现单元内。
///
/// 句柄均为非零整数，返回零表示失败或空句柄；重复注销零句柄或已失效句柄恒为安全操作。
/// 该门面与编译期插桩开关无关，拿到预编译 Release 静态库的下游无需重编库即可使用。
///
struct FrameMonitor {
    /// @brief 注册帧完成回调。
    /// @param callback 每批帧读数到来时调用的函数；空函数被拒绝。
    /// @return 非零订阅句柄；被拒绝时返回零。
    [[nodiscard]] static auto subscribe(std::function<void(const FrameBatch &)> callback) -> std::uint64_t;

    /// @brief 注销帧完成回调，重复注销或句柄失效均安全。
    /// @param subscription `subscribe` 返回的句柄。
    /// @return 找到并移除时为真；未找到时为假。
    [[nodiscard]] static auto unsubscribe(std::uint64_t subscription) -> bool;

    /// @brief 按名称唯一注册一个自定义指标，重名不覆盖既有读数。
    /// @param name 指标名，须非空且不含控制字符。
    /// @return 非零指标句柄；名称非法或重名时返回零，原因由 `last_metric_error` 给出。
    [[nodiscard]] static auto register_metric(const std::string &name) -> std::uint64_t;

    /// @brief 读取最近一次命名指标注册的结果。
    /// @return 最近一次注册的错误原因，成功时为 `None`。
    [[nodiscard]] static auto last_metric_error() -> MetricError;

    /// @brief 写入自定义指标当前读数。
    /// @param metric `register_metric` 返回的句柄。
    /// @param value 指标当前值。
    /// @return 句柄有效时为真；失效或已注销时为假。
    [[nodiscard]] static auto set_metric(std::uint64_t metric, double value) -> bool;

    /// @brief 读取自定义指标当前读数。
    /// @param metric `register_metric` 返回的句柄。
    /// @return 当前读数；句柄失效时返回零。
    [[nodiscard]] static auto metric_value(std::uint64_t metric) -> double;

    /// @brief 注销自定义指标，重复注销安全。
    /// @param metric `register_metric` 返回的句柄。
    /// @return 找到并移除时为真；未找到时为假。
    [[nodiscard]] static auto unregister_metric(std::uint64_t metric) -> bool;

    /// @brief 开始一个下游命名的作用域计时。
    ///
    /// 零订阅者时立即返回空句柄，不构造对象也不读取单调钟。
    /// @param name 作用域名，同名作用域在快照里聚合总耗时与调用次数。
    /// @return 非零作用域句柄；零订阅者时返回零（空句柄）。
    [[nodiscard]] static auto begin_scope(const std::string &name) -> std::uint64_t;

    /// @brief 结束作用域计时并累加到同名聚合，重复结束安全。
    /// @param scope `begin_scope` 返回的句柄，零句柄被忽略。
    static auto end_scope(std::uint64_t scope) -> void;

    /// @brief 导出当前门面状态的结构化快照。
    /// @return JSON 对象文本，含订阅者数、命名指标、作用域聚合与诊断计数。
    [[nodiscard]] static auto snapshot_json() -> std::string;

    /// @brief 读取运行期诊断计数，调用本身不产生采集开销。
    /// @return 当前诊断计数的值拷贝。
    [[nodiscard]] static auto diagnostics() -> MonitorDiagnostics;

    /// @brief 单个帧末批量允许容纳的最大读数条数。
    /// @return 固定容量上限。
    [[nodiscard]] static auto max_batch_frames() -> std::size_t;
};

namespace detail {

/// @brief 帧循环帧末入口，由界面线程在帧统计定型后调用；无订阅者时立即返回。
/// @param frame_delta 帧循环传入的间隔，与 `FrameStats::record` 同一口径（秒）。
/// @param idle 本帧是否为无实际渲染的 idle 帧。
auto publish_frame_sample(double frame_delta, bool idle) -> void;

}  // namespace detail

}  // namespace aurora
