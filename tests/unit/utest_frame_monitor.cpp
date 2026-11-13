/// 测试类型: unit
/// 目标单元: include/aurora/perf/frame_monitor.h
/// 测试说明: 覆盖下游监控订阅门面——零订阅时四项采集计数不增长且作用域计时返回空句柄、
/// 两个订阅者收到同一批读数、注销后不再回调、回调期间注销只对下一批生效、重复注销与失效
/// 句柄安全、命名指标重名报明确错误且不覆盖、序列化可被既有 JSON 解析器读回、异常订阅者
/// 积压到上限后只增加丢弃计数、作用域计时聚合进入快照。进程级状态在每个用例前后显式清理。

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "aurora/core/json.h"
#include "aurora/perf/frame_monitor.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_frame_monitor {

namespace {

/// @brief 本文件注册过的全部句柄；订阅与指标共用一张表，清理时两种注销入口分别尝试。
auto tracked() -> std::vector<std::uint64_t> & {
    static std::vector<std::uint64_t> handles;
    return handles;
}

/// @brief 撤销本文件注册的全部句柄，使进程级单例在用例之间不串味。
auto cleanup() -> void {
    for (const std::uint64_t handle : tracked()) {
        (void)FrameMonitor::unsubscribe(handle);
        (void)FrameMonitor::unregister_metric(handle);
    }
    tracked().clear();
}

/// @brief 注册订阅者并登记到清理表。
auto add_subscriber(std::function<void(const FrameBatch &)> callback) -> std::uint64_t {
    const std::uint64_t handle = FrameMonitor::subscribe(std::move(callback));
    tracked().push_back(handle);
    return handle;
}

/// @brief 注册命名指标并登记到清理表。
auto add_metric(const std::string &name) -> std::uint64_t {
    const std::uint64_t handle = FrameMonitor::register_metric(name);
    tracked().push_back(handle);
    return handle;
}

}  // namespace

AURORA_TEST_CASE(zero_subscribers_add_no_collection_cost) {
    cleanup();
    AURORA_TEST_CHECK_EQ(FrameMonitor::diagnostics().subscriber_count, std::uint64_t{0});

    const MonitorDiagnostics before = FrameMonitor::diagnostics();
    const std::uint64_t scope = FrameMonitor::begin_scope("unused-zone");
    AURORA_TEST_CHECK_EQ(scope, std::uint64_t{0});
    AURORA_TEST_CHECK_NO_THROW(FrameMonitor::end_scope(0));
    for (int i = 0; i < 8; ++i) {
        AURORA_TEST_CHECK_NO_THROW(aurora::detail::publish_frame_sample(0.016, false));
    }
    const MonitorDiagnostics after = FrameMonitor::diagnostics();

    AURORA_TEST_CHECK_EQ(after.hook_registrations, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(after.heap_allocations, before.heap_allocations);
    AURORA_TEST_CHECK_EQ(after.clock_reads, before.clock_reads);
    AURORA_TEST_CHECK_EQ(after.bytes_emitted, before.bytes_emitted);
    AURORA_TEST_CHECK_EQ(after.dropped_batches, before.dropped_batches);
    cleanup();
}

AURORA_TEST_CASE(two_subscribers_receive_identical_batch) {
    cleanup();
    std::size_t seen_a = 0;
    std::size_t seen_b = 0;
    FrameSample first{};
    FrameSample second{};

    (void)add_subscriber([&](const FrameBatch &batch) {
        ++seen_a;
        if (batch.count > 0) {
            first = *batch.samples;
        }
    });
    (void)add_subscriber([&](const FrameBatch &batch) {
        ++seen_b;
        if (batch.count > 0) {
            second = *batch.samples;
        }
    });

    aurora::detail::publish_frame_sample(0.016, false);
    AURORA_TEST_CHECK_EQ(seen_a, std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen_b, std::size_t{1});
    AURORA_TEST_CHECK_EQ(first.index, second.index);
    AURORA_TEST_CHECK_EQ(first.frame_delta, 0.016);
    AURORA_TEST_CHECK_EQ(second.frame_delta, 0.016);
    AURORA_TEST_CHECK_FALSE(first.idle);
    cleanup();
}

AURORA_TEST_CASE(unsubscribe_stops_callbacks_and_is_idempotent) {
    cleanup();
    std::size_t seen = 0;
    const std::uint64_t handle = FrameMonitor::subscribe([&](const FrameBatch &) { ++seen; });
    AURORA_TEST_CHECK_TRUE(handle != 0);

    aurora::detail::publish_frame_sample(0.010, false);
    AURORA_TEST_CHECK_EQ(seen, std::size_t{1});

    AURORA_TEST_CHECK_TRUE(FrameMonitor::unsubscribe(handle));
    aurora::detail::publish_frame_sample(0.010, false);
    AURORA_TEST_CHECK_EQ(seen, std::size_t{1});
    AURORA_TEST_CHECK_FALSE(FrameMonitor::unsubscribe(handle));
    AURORA_TEST_CHECK_FALSE(FrameMonitor::unsubscribe(0));
    cleanup();
}

AURORA_TEST_CASE(callback_unsubscribe_applies_to_next_batch) {
    cleanup();
    std::size_t seen_a = 0;
    std::size_t seen_b = 0;
    std::uint64_t handle_b = 0;

    (void)add_subscriber([&](const FrameBatch &) {
        ++seen_a;
        (void)FrameMonitor::unsubscribe(handle_b);
    });
    handle_b = add_subscriber([&](const FrameBatch &) { ++seen_b; });

    aurora::detail::publish_frame_sample(0.010, false);
    AURORA_TEST_CHECK_EQ(seen_a, std::size_t{1});
    AURORA_TEST_CHECK_EQ(seen_b, std::size_t{1});

    aurora::detail::publish_frame_sample(0.010, false);
    AURORA_TEST_CHECK_EQ(seen_a, std::size_t{2});
    AURORA_TEST_CHECK_EQ(seen_b, std::size_t{1});
    cleanup();
}

AURORA_TEST_CASE(duplicate_metric_reports_error_without_overwrite) {
    cleanup();
    const std::uint64_t metric = add_metric("frame-budget");
    AURORA_TEST_CHECK_TRUE(metric != 0);
    AURORA_TEST_CHECK_TRUE(FrameMonitor::last_metric_error() == MetricError::None);
    AURORA_TEST_CHECK_TRUE(FrameMonitor::set_metric(metric, 16.5));
    AURORA_TEST_CHECK_EQ(FrameMonitor::metric_value(metric), 16.5);

    const std::uint64_t duplicate = FrameMonitor::register_metric("frame-budget");
    AURORA_TEST_CHECK_EQ(duplicate, std::uint64_t{0});
    AURORA_TEST_CHECK_TRUE(FrameMonitor::last_metric_error() == MetricError::DuplicateName);
    AURORA_TEST_CHECK_EQ(FrameMonitor::metric_value(metric), 16.5);

    const std::uint64_t invalid = FrameMonitor::register_metric("");
    AURORA_TEST_CHECK_EQ(invalid, std::uint64_t{0});
    AURORA_TEST_CHECK_TRUE(FrameMonitor::last_metric_error() == MetricError::InvalidName);

    const std::string text = FrameMonitor::snapshot_json();
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("frame-budget")"));
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("value": 16.5)"));
    const auto parsed = aurora::json::parse(text);
    AURORA_TEST_CHECK_TRUE(parsed.ok());

    AURORA_TEST_CHECK_TRUE(FrameMonitor::unregister_metric(metric));
    AURORA_TEST_CHECK_FALSE(FrameMonitor::unregister_metric(metric));
    AURORA_TEST_CHECK_FALSE(FrameMonitor::set_metric(metric, 1.0));
    AURORA_TEST_CHECK_EQ(FrameMonitor::metric_value(metric), 0.0);
    cleanup();
}

AURORA_TEST_CASE(slow_subscriber_only_increments_drop_count) {
    cleanup();
    const MonitorDiagnostics before = FrameMonitor::diagnostics();
    (void)add_subscriber([](const FrameBatch &) { throw std::runtime_error("slow subscriber"); });

    const std::size_t frames = FrameMonitor::max_batch_frames() + 2;
    for (std::size_t i = 0; i < frames; ++i) {
        AURORA_TEST_CHECK_NO_THROW(aurora::detail::publish_frame_sample(0.010, false));
    }
    const MonitorDiagnostics after = FrameMonitor::diagnostics();
    AURORA_TEST_CHECK_TRUE(after.dropped_batches > before.dropped_batches);
    AURORA_TEST_CHECK_EQ(after.bytes_emitted, before.bytes_emitted);

    const std::uint64_t healthy = add_subscriber([](const FrameBatch &) {});
    AURORA_TEST_CHECK_TRUE(healthy != 0);
    cleanup();
}

AURORA_TEST_CASE(scope_timing_needs_a_subscriber) {
    cleanup();
    const std::uint64_t before_reads = FrameMonitor::diagnostics().clock_reads;
    AURORA_TEST_CHECK_EQ(FrameMonitor::begin_scope("no-subscriber"), std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(FrameMonitor::diagnostics().clock_reads, before_reads);

    (void)add_subscriber([](const FrameBatch &) {});
    const std::uint64_t scope = FrameMonitor::begin_scope("decode-image");
    AURORA_TEST_CHECK_TRUE(scope != 0);
    FrameMonitor::end_scope(scope);
    FrameMonitor::end_scope(scope);
    AURORA_TEST_CHECK_EQ(FrameMonitor::diagnostics().clock_reads, before_reads + 2);

    const std::string text = FrameMonitor::snapshot_json();
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("scopes")"));
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("decode-image")"));
    cleanup();
}

AURORA_TEST_CASE(snapshot_exposes_diagnostics_in_machine_readable_form) {
    cleanup();
    const std::string text = FrameMonitor::snapshot_json();
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("diagnostics")"));
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("subscriber_count": 0)"));
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("hook_registrations")"));
    AURORA_TEST_CHECK_THAT(text, ::aurora::testing::matchers::has_substr(R"("bytes_emitted")"));
    const auto parsed = aurora::json::parse(text);
    AURORA_TEST_CHECK_TRUE(parsed.ok());
    cleanup();
}

}  // namespace aurora::test_cases::utest_frame_monitor
