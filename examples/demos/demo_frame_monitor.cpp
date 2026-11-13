/// @brief 演示下游应用使用恒开性能监控门面订阅帧读数、登记自定义指标并导出快照。
/// @file demo_frame_monitor.cpp
///
/// 帧读数由应用的帧循环在帧末自动投递；本示例只展示订阅、命名指标、作用域计时与
/// 快照导出四个入口，不需要打开编译期插桩开关，也不需要重编 Aurora 库。
///

#include <cstdint>
#include <string>

#include "aurora/aurora.h"

namespace {

auto run() -> int {
    std::uint64_t frames_seen = 0;
    const std::uint64_t subscription = aurora::FrameMonitor::subscribe(
        [&frames_seen](const aurora::FrameBatch &batch) { frames_seen += batch.count; });

    const std::uint64_t metric = aurora::FrameMonitor::register_metric("app-frame-budget-ms");
    (void)aurora::FrameMonitor::set_metric(metric, 16.0);

    const std::uint64_t scope = aurora::FrameMonitor::begin_scope("demo-work");
    const double budget = aurora::FrameMonitor::metric_value(metric);
    aurora::FrameMonitor::end_scope(scope);

    AURORA_LOG_RAW("demo", "budget_ms=", budget, " frames_seen=", frames_seen, "\n");
    AURORA_LOG_RAW("demo", aurora::FrameMonitor::snapshot_json(), "\n");

    (void)aurora::FrameMonitor::unregister_metric(metric);
    (void)aurora::FrameMonitor::unsubscribe(subscription);
    return 0;
}

}  // namespace

auto main() -> int {
    try {
        return run();
    } catch (...) {
        return 1;
    }
}
