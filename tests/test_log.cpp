// ── API 覆盖映射 ─────────────────────────────
// core/log.h（Logger 级别/通道/sink 捕获）。

#include <string>

#include "aurora/core/diagnostics.h"
#include "aurora/core/log.h"
#include "test_harness.h"

AURORA_TEST() {
    using aurora::Logger;
    using aurora::LogLevel;

    // 捕获 sink 输出，便于断言（测试用，单线程）。
    std::string captured;
    Logger::instance().set_sink([&](std::string_view line) -> void { captured += line; });
    Logger::instance().set_enabled(true);

    // 1) 级别过滤：默认 Info，低于 Info 的 Trace/Debug 应被丢弃。
    captured.clear();
    Logger::instance().set_level(LogLevel::Info);
    AURORA_LOG_TRACE("test", "should-be-filtered");
    AURORA_LOG_DEBUG("test", "should-be-filtered");
    AURORA_LOG_INFO("test", "visible-info");
    AURORA_LOG_WARN("test", "visible-warn");
    AURORA_TEST_CHECK(captured.find("should-be-filtered") == std::string::npos);
    AURORA_TEST_CHECK(captured.find("visible-info") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("visible-warn") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("[INF]") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("[WRN]") != std::string::npos);

    // 2) 格式：[YYYY-MM-DD HH:MM:SS][LVL][category@threadId file:line] > message
    captured.clear();
    AURORA_LOG_ERROR("net", "connection lost");
    AURORA_TEST_CHECK(captured.find("[ERR]") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("[net@") != std::string::npos);  // 分类 + 线程 id
    AURORA_TEST_CHECK(captured.find("test_log.cpp:") != std::string::npos);  // file:line
    AURORA_TEST_CHECK(captured.find("] > connection lost") != std::string::npos);
    AURORA_TEST_CHECK(captured.back() == '\n');

    // 3) 降低级别后 Trace/Debug 也可见。
    captured.clear();
    Logger::instance().set_level(LogLevel::Trace);
    AURORA_LOG_TRACE("dbg", "fine-grained");
    AURORA_LOG_DEBUG("dbg", "debug-line");
    AURORA_TEST_CHECK(captured.find("[TRC]") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("[DBG]") != std::string::npos);

    // 4) 禁用后完全静默。
    Logger::instance().set_enabled(false);
    captured.clear();
    AURORA_LOG_FATAL("x", "must-not-appear");
    AURORA_TEST_CHECK(captured.empty());

    // 5) Diagnostics 桥接到 Logger：warn -> Warn，degraded -> Warn（severity 由 slug 表驱动）。
    //    运行时 category/severity 取自 g_error_table 的 slug 映射，而非调用方传入的 where 字符串；
    //    where 字段保留调用方传入的 where 原值。此处使用已注册的 slug 以保证断言可精确匹配。
    Logger::instance().set_enabled(true);
    Logger::instance().set_level(LogLevel::Trace);
    captured.clear();
    aurora::Diagnostics::warn("suspicious state", "widget", "layout-invalid-constraints");
    aurora::Diagnostics::degraded("negative padding clamped", "layout", "render-degraded");
    // Diagnostics 桥接到 Logger 时以 JSON 行输出（to_json_line）：校验 JSON 字段而非纯文本格式。
    AURORA_TEST_CHECK(captured.find("\"severity\":\"warning\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"category\":\"layout\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"message\":\"suspicious state\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"where\":\"widget\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"category\":\"render\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"message\":\"negative padding clamped\"") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("\"where\":\"layout\"") != std::string::npos);

    // 6) Diagnostics 内存收集仍可用（take/count）。
    const auto logs = aurora::Diagnostics::take();
    AURORA_TEST_CHECK(!logs.empty());
    AURORA_TEST_CHECK(aurora::Diagnostics::count() == 0);

    // 7) 可变参数：类型安全的流式拼接（向后兼容单参数形式）。
    captured.clear();
    AURORA_LOG_WARN("var", "retries=", 3, " reason=", std::string("timeout"));
    AURORA_TEST_CHECK(captured.find("[WRN]") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("] > retries=3 reason=timeout") != std::string::npos);
    captured.clear();
    AURORA_LOG_INFO("var", "single");  // 单参数仍兼容
    AURORA_TEST_CHECK(captured.find("[INF]") != std::string::npos);
    AURORA_TEST_CHECK(captured.find("] > single") != std::string::npos);

    // 复位默认 sink，避免影响其他进程。
    Logger::instance().set_sink(nullptr);
    Logger::instance().set_level(LogLevel::Info);
    Logger::instance().set_enabled(true);

    AURORA_LOG_INFO("test", "log_test: OK");
}
