#pragma once

/// @brief 下游性能监控门面的机器可读登记来源，供生成器写出性能契约段。
/// @file perf_api.h
///
/// 该表是 `aurora_api.json` 中 perf 段的唯一事实来源：新增或调整门面入口时改这里，
/// 再刷新生成目标，不手工改写入库文件。字段以英文描述，供工具链与代码生成消费。
///

#include <utility>

#include "aurora/core/json.h"

namespace aurora::tools {

/// @brief 构造性能门面契约段。
/// @return perf 段的 JSON 对象。
[[nodiscard]] inline auto perf_api_section() -> aurora::json::Value {
    auto section = aurora::json::Value::object();
    section.set("header", aurora::json::Value{"aurora/perf/frame_monitor.h"});
    section.set("entry", aurora::json::Value{"FrameMonitor"});
    section.set("availability", aurora::json::Value{"always-on"});
    section.set("profiling_dependency", aurora::json::Value{"none"});
    section.set("release_usable_without_rebuild", aurora::json::Value{true});
    section.set("max_batch_frames", aurora::json::Value{64});

    auto functions = aurora::json::Value::array();
    const char *const names[] = {"subscribe",  "unsubscribe",   "register_metric",   "last_metric_error",
                                 "set_metric", "metric_value",  "unregister_metric", "begin_scope",
                                 "end_scope",  "snapshot_json", "diagnostics",       "max_batch_frames"};
    for (const char *name : names) {
        auto item = aurora::json::Value::object();
        item.set("name", aurora::json::Value{name});
        functions.push_back(std::move(item));
    }
    section.set("functions", std::move(functions));

    auto zero = aurora::json::Value::object();
    zero.set("hook_registrations", aurora::json::Value{0});
    zero.set("heap_allocations", aurora::json::Value{0});
    zero.set("clock_reads", aurora::json::Value{0});
    zero.set("bytes_emitted", aurora::json::Value{0});
    section.set("zero_subscriber_contract", std::move(zero));

    auto diagnostics = aurora::json::Value::array();
    const char *const keys[] = {"hook_registrations", "heap_allocations", "clock_reads",
                                "bytes_emitted",      "dropped_batches",  "subscriber_count"};
    for (const char *key : keys) {
        diagnostics.push_back(aurora::json::Value{key});
    }
    section.set("diagnostics", std::move(diagnostics));
    return section;
}

}  // namespace aurora::tools
