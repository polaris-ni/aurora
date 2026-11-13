/// @brief 独立消费探针：以 find_package 链接的安装产物验证恒开门面无需重编库即可使用。
/// @file main.cpp
///
/// 不依赖 `AURORA_ENABLE_PROFILING`，不引用源码树内的头，也不链接任何遥测组件。
/// 退出码 0 表示订阅、命名指标与快照导出在安装产物上全部可用。
///

#include <cstdint>

#include "aurora/aurora.h"

namespace {

auto run() -> int {
    const std::uint64_t metric = aurora::FrameMonitor::register_metric("consumer-metric");
    if (metric == 0) {
        return 2;
    }
    (void)aurora::FrameMonitor::set_metric(metric, 42.0);

    const std::uint64_t subscription = aurora::FrameMonitor::subscribe([](const aurora::FrameBatch &) {});
    if (subscription == 0) {
        return 3;
    }

    const aurora::MonitorDiagnostics diag = aurora::FrameMonitor::diagnostics();
    AURORA_LOG_RAW("probe", "metric=", aurora::FrameMonitor::metric_value(metric),
                   " subscribers=", diag.subscriber_count, "\n");
    AURORA_LOG_RAW("probe", aurora::FrameMonitor::snapshot_json(), "\n");

    (void)aurora::FrameMonitor::unsubscribe(subscription);
    (void)aurora::FrameMonitor::unregister_metric(metric);
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
