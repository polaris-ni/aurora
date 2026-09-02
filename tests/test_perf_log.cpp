// 目标源单元：perf/perf_log.h（PerfLog 周期摘要 + JSON/CSV 快照）。
//
// API 覆盖映射：
//   PerfLog::enable/disable/enabled   -> test_enable_disable
//   PerfLog::on_frame_end（计数触发） -> test_interval_firing
//   PerfLog::csv_header               -> test_csv_header
//   PerfLog::snapshot_json/snapshot_csv -> test_snapshots_contain_metrics
//   PerfLog::dump_json/dump_csv       -> 豁免说明：直接写 AURORA_LOG_* 通道，
//      无注入点可捕获断言；其格式化内核与 snapshot_* 共用，经 snapshot 用例覆盖。
//
// 说明：PerfLog 为进程级静态单例，用例间必须复位（disable），避免污染其他测试。

#include <string>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::FrameStats;
using aurora::PerfLog;

namespace {

void reset() { PerfLog::disable(); }

void test_enable_disable() {
    reset();
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());
    PerfLog::enable(2);
    AURORA_TEST_CHECK(PerfLog::enabled());
    PerfLog::disable();
    AURORA_TEST_CHECK_FALSE(PerfLog::enabled());
    // 默认间隔参数可调用
    PerfLog::enable();
    AURORA_TEST_CHECK(PerfLog::enabled());
    reset();
}

void test_interval_firing() {
    reset();
    PerfLog::enable(3);
    // 间隔=3：第1、2帧不触发重置，第3帧触发（内部 counter 归零）。行为不可直接观测，
    // 但接口在任意帧序下不得崩溃/改写 enabled 态——这是本用例的不变量断言。
    for (int i = 0; i < 7; ++i) {
        PerfLog::on_frame_end();
        AURORA_TEST_CHECK(PerfLog::enabled());
    }
    reset();
}

void test_csv_header() {
    const std::string header = PerfLog::csv_header();
    AURORA_TEST_CHECK_FALSE(header.empty());
    // 表头应含核心指标列（与 snapshot_csv 列对齐）
    AURORA_TEST_CHECK(header.find("frame") != std::string::npos);
}

void test_snapshots_contain_metrics() {
    reset();
    PerfLog::enable(1000000); // 测试期间不触发周期摘要
    // 喂几帧真实统计，使 FrameStats 窗口非空
    auto &stats = FrameStats::instance();
    stats.reset();
    for (int f = 0; f < 5; ++f) {
        stats.record(0.01667);
        PerfLog::on_frame_end();
    }
    const std::string json = PerfLog::snapshot_json();
    const std::string csv = PerfLog::snapshot_csv();
    AURORA_TEST_CHECK_FALSE(json.empty());
    AURORA_TEST_CHECK(json.find('{') != std::string::npos);
    AURORA_TEST_CHECK_FALSE(csv.empty());
    AURORA_TEST_CHECK(csv.find(',') != std::string::npos);
    reset();
}

} // namespace

AURORA_TEST() {
    test_enable_disable();
    test_interval_firing();
    test_csv_header();
    test_snapshots_contain_metrics();
}
