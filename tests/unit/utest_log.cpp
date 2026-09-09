/// 测试类型: unit
/// 目标单元: include/aurora/core/log.h
/// 测试说明: 日志级别标签、时间戳格式、级别阈值过滤与 enabled 开关、raw
/// 无前缀通道、日志行格式（前缀/模块/file:line/换行）、log_concat 变参拼接、sink 恢复与重捕获、init_console 可调用性

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/log.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_log {

namespace m = aurora::testing::matchers;

namespace {
/// @brief RAII：接管 Logger 的 sink / raw_sink 捕获输出；析构时按「先读原值」恢复
/// 级别、开关与默认 sink，保证不把 Logger 全局状态泄漏给其他用例。
class CapturedLogger {
  public:
    CapturedLogger() : level_{aurora::Logger::instance().level()}, enabled_{aurora::Logger::instance().is_enabled()} {
        auto& logger = aurora::Logger::instance();
        logger.set_sink([this](std::string_view line) -> void { lines.emplace_back(line); });
        logger.set_raw_sink([this](std::string_view text) -> void { raw_lines.emplace_back(text); });
    }
    ~CapturedLogger() {
        auto& logger = aurora::Logger::instance();
        logger.set_level(level_);
        logger.set_enabled(enabled_);
        logger.set_sink(nullptr);  // 恢复默认 stderr
        logger.set_raw_sink(nullptr);  // 恢复默认 stdout
    }
    CapturedLogger(const CapturedLogger&) = delete;
    auto operator=(const CapturedLogger&) -> CapturedLogger& = delete;
    CapturedLogger(CapturedLogger&&) = delete;
    auto operator=(CapturedLogger&&) -> CapturedLogger& = delete;

    std::vector<std::string> lines;  // NOLINT(*-non-private-member-variables-in-classes) 诊断日志行（含前缀与换行）
    std::vector<std::string> raw_lines;  // NOLINT(*-non-private-member-variables-in-classes) raw 功能输出（无前缀）

  private:
    aurora::LogLevel level_;
    bool enabled_;
};
}  // namespace

AURORA_TEST_CASE(log_level_label_covers_all_levels) {
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Trace), "TRC");
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Debug), "DBG");
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Info), "INF");
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Warn), "WRN");
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Error), "ERR");
    AURORA_TEST_CHECK_STREQ(log_level_label(LogLevel::Fatal), "FTL");
}

AURORA_TEST_CASE(log_timestamp_has_fixed_shape) {
    // log_timestamp() 固定输出 YYYY-MM-DD HH:MM:SS（本地时间）。
    const auto ts = log_timestamp();
    AURORA_TEST_REQUIRE_THAT(ts, m::size_is(19));
    AURORA_TEST_CHECK_EQ(ts[4], '-');
    AURORA_TEST_CHECK_EQ(ts[7], '-');
    AURORA_TEST_CHECK_EQ(ts[10], ' ');
    AURORA_TEST_CHECK_EQ(ts[13], ':');
    AURORA_TEST_CHECK_EQ(ts[16], ':');
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;  // 分隔位
        }
        AURORA_TEST_CHECK(std::isdigit(static_cast<unsigned char>(ts[i])) != 0);
    }
}

AURORA_TEST_CASE(level_threshold_filters_below) {
    CapturedLogger capture;
    aurora::Logger::instance().set_level(LogLevel::Warn);
    AURORA_LOG_INFO("utest", "below threshold");  // 低于阈值：丢弃
    AURORA_LOG_WARN("utest", "at threshold");
    AURORA_LOG_ERROR("utest", "above threshold");
    AURORA_TEST_CHECK_EQ(capture.lines.size(), std::size_t{2});
    AURORA_TEST_CHECK_THAT(capture.lines[0], m::has_substr("[WRN]"));
    AURORA_TEST_CHECK_THAT(capture.lines[0], m::has_substr("at threshold"));
    AURORA_TEST_CHECK_THAT(capture.lines[1], m::has_substr("[ERR]"));
}

AURORA_TEST_CASE(set_enabled_false_silences_diagnostics) {
    CapturedLogger capture;
    aurora::Logger::instance().set_enabled(false);
    AURORA_LOG_FATAL("utest", "should be dropped");  // 禁用后所有诊断日志静默丢弃
    AURORA_TEST_CHECK(capture.lines.empty());
}

AURORA_TEST_CASE(raw_channel_bypasses_threshold_and_prefix) {
    CapturedLogger capture;
    aurora::Logger::instance().set_level(LogLevel::Fatal);  // 最高阈值也不影响 raw 通道
    AURORA_LOG_RAW("cli", "{\"ok\":", 1, "}\n");
    // raw 为功能输出：不加时间戳/级别/分类前缀，逐字节等于拼接结果，调用方自负换行。
    AURORA_TEST_REQUIRE_EQ(capture.raw_lines.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(capture.raw_lines[0], std::string{"{\"ok\":1}\n"});
    AURORA_TEST_CHECK(capture.lines.empty());  // 诊断通道未产生输出
}

AURORA_TEST_CASE(log_line_format_prefix_module_and_newline) {
    CapturedLogger capture;
    AURORA_LOG_WARN("utest", "hello ", "world");
    AURORA_TEST_REQUIRE_EQ(capture.lines.size(), std::size_t{1});
    const auto& line = capture.lines[0];
    AURORA_TEST_CHECK(line.starts_with('['));  // [YYYY-MM-DD HH:MM:SS]
    AURORA_TEST_CHECK_THAT(line, m::has_substr("[WRN]"));
    AURORA_TEST_CHECK_THAT(line, m::has_substr("[utest@"));  // [category@threadId
    AURORA_TEST_CHECK_THAT(line, m::has_substr("utest_log.cpp:"));  // file:line 归属（宏自动填入）
    AURORA_TEST_CHECK_THAT(line, m::ends_with("hello world\n"));  // 消息 + 换行收尾
}

AURORA_TEST_CASE(log_concat_folds_mixed_types) {
    // 无参数退化形式：允许 AURORA_LOG_*(category) 的向后兼容调用。
    AURORA_TEST_CHECK_EQ(aurora::detail::log_concat(), std::string{});
    AURORA_TEST_CHECK_EQ(aurora::detail::log_concat("a", 1, " ", 2.5), std::string{"a1 2.5"});
    AURORA_TEST_CHECK_EQ(aurora::detail::log_concat(std::string{"only"}), std::string{"only"});
}

AURORA_TEST_CASE(sink_restore_and_recapture_works) {
    {
        CapturedLogger capture;
        AURORA_LOG_INFO("utest", "first");
        AURORA_TEST_CHECK_EQ(capture.lines.size(), std::size_t{1});
    }  // 析构：恢复级别/开关/默认 sink
    {
        // 恢复后可再次接管：说明 set_sink(nullptr) 的恢复路径无损。
        CapturedLogger recapture;
        AURORA_LOG_INFO("utest", "second");
        AURORA_TEST_CHECK_EQ(recapture.lines.size(), std::size_t{1});
        AURORA_TEST_CHECK_THAT(recapture.lines[0], m::has_substr("second"));
    }
    AURORA_TEST_CHECK_NO_THROW(aurora::init_console());  // noexcept 安全网：可重复调用
}

AURORA_TEST_CASE(low_threshold_passes_trace_and_debug) {
    CapturedLogger capture;
    aurora::Logger::instance().set_level(LogLevel::Trace);
    AURORA_LOG_TRACE("utest", "tr");
    AURORA_LOG_DEBUG("utest", "db");
    AURORA_TEST_CHECK_EQ(capture.lines.size(), std::size_t{2});
    AURORA_TEST_CHECK_THAT(capture.lines[0], m::has_substr("[TRC]"));
    AURORA_TEST_CHECK_THAT(capture.lines[1], m::has_substr("[DBG]"));
}

}  // namespace aurora::test_cases::utest_log
