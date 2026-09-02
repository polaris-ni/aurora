#pragma once
#include <chrono>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>

#include "aurora/core/platform.h"
#include "aurora/core/string_util.h"

namespace aurora {

/**
 * @brief 日志级别（规格：日志打印模块）。
 *
 * 级别自低向高：Trace < Debug < Info < Warn < Error < Fatal。
 * 全局 Logger 只输出 >= 当前阈值的日志（默认 Info）。
 */
enum class LogLevel : std::uint8_t {
    Trace = 0u,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

/// @brief 级别转短标签（TRC/DBG/INF/WRN/ERR/FTL）。
[[nodiscard]] inline auto log_level_label(LogLevel level) noexcept -> std::string_view {
    switch (level) {
    case LogLevel::Trace: return "TRC";
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INF";
    case LogLevel::Warn: return "WRN";
    case LogLevel::Error: return "ERR";
    case LogLevel::Fatal: return "FTL";
    }
    return "???";
}

/**
 * @brief 格式化时间戳为 `YYYY-MM-DD HH:MM:SS`（本地时间）。
 * @note 单线程 UI 假设；内部用 `std::chrono` + `std::localtime`，无锁。
 */
[[nodiscard]] inline auto log_timestamp() noexcept -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef AURORA_PLATFORM_WINDOWS
    localtime_s(&tm, &t); // NOLINT：Windows 安全变体
#else
    localtime_r(&t, &tm); // NOLINT：POSIX 安全变体
#endif

    return aurora::internal::string_format("%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                                           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/**
 * @brief 日志输出目标（sink）签名。
 * @param line 已格式化的完整日志行（含换行符）。
 */
using LogSink = std::function<void(std::string_view line)>;

/**
 * @brief 初始化控制台为 UTF-8 代码页（仅 Windows 生效，其它平台为空实现）。
 *
 * 默认 Windows 控制台以 GBK(CP936) 解释 UTF-8 字节，导致源码中的中文输出乱码。
 * 调用一次 `SetConsoleOutputCP(CP_UTF8)` 即可让 `cout`/`cerr`/`fprintf` 等全部控制台
 * 输出按 UTF-8 正确渲染。建议在 `main()` 第一行调用；`Logger` 也会在首次使用时自动触发。
 */
auto init_console() noexcept -> void;

/**
 * @brief 全局日志器（单例）。
 *
 * 特性：
 * - 级别阈值过滤（默认 Info）。
 * - 可重定向 sink（默认 stderr），便于测试捕获或文件落盘。
 * - 统一格式：`[YYYY-MM-DD HH:MM:SS][LEVEL][module@threadId filename:line] > content`。
 * - 单线程 UI 假设：内部状态无锁（仅 sink 调用为外部回调，由调用方保证线程安全）。
 * - 类型安全的可变参数消息：经 `AURORA_LOG_*` 宏传入任意数量参数，按 `operator<<` 折叠拼接。
 *
 * 推荐用法：通过 `AURORA_LOG_*` 宏记录，自动填入 file:line。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
class Logger {
  public:
    /// @brief 取得全局唯一实例。
    static auto instance() -> Logger &;

    /// @brief 设置最低输出级别（低于此级别的日志被丢弃）。
    auto set_level(LogLevel level) noexcept -> void;

    /// @brief 当前最低输出级别。
    [[nodiscard]] auto level() const noexcept -> LogLevel;

    /// @brief 设置输出目标；传 nullptr 恢复默认 stderr。
    auto set_sink(LogSink sink) -> void;

    /// @brief 启用/禁用日志（禁用后所有日志静默丢弃）。
    auto set_enabled(bool enabled) noexcept -> void;

    /// @brief 是否启用。
    [[nodiscard]] auto is_enabled() const noexcept -> bool;

    /// @brief 记录一条日志（供宏调用，自动带 file:line）。
    auto log(std::string_view file, int line_no, LogLevel level, std::string_view category,
             std::string_view message) const -> void;

    /**
     * @brief 无前缀纯文本输出通道（功能输出，非日志）。
     *
     * 直接写入 raw sink（默认 stdout），**不经过级别阈值过滤、不加时间戳/级别/分类前缀**，
     * 调用方自负换行与协议格式。用于：CLI 的 JSON 结果 / usage 文本、benchmark 表格、
     * LSP/MCP 等基于 stdio 的线协议帧（需精确字节，如 `\\r\\n\\r\\n`）。
     * 与诊断日志区分：诊断/错误/警告请用 `AURORA_LOG_*` 系列。
     */
    auto raw(std::string_view category, std::string_view message) const -> void;

    /// @brief 设置 raw 通道（功能输出）目标；传 nullptr 恢复默认 stdout。
    auto set_raw_sink(LogSink sink) -> void;

  private:
    Logger() = default;

    static auto default_sink() -> LogSink;
    static auto default_raw_sink() -> LogSink;

    LogLevel m_level = LogLevel::Info;
    bool m_enabled = true;
    LogSink m_sink = default_sink();
    LogSink m_raw_sink = default_raw_sink();
};

namespace detail {

/// @brief 无参数：返回空消息（允许 `AURORA_LOG_*(category)` 调用，向后兼容退化形式）。
[[nodiscard]] inline auto log_concat() -> std::string { return {}; }

/**
 * @brief 把任意数量参数经 `operator<<` 折叠拼接为字符串（类型安全，无需格式化库）。
 *
 * 供 `AURORA_LOG_*` 宏的可变参数形态使用；单参数时退化为原样输出（与旧单 `msg` 行为一致）。
 */
template<typename... Args> [[nodiscard]] auto log_concat(Args &&...args) -> std::string {
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    return std::move(oss).str();
}

} // namespace detail

} // namespace aurora

#ifdef __FILE_NAME__
#define AURORA_FILE_NAME __FILE_NAME__
#else
#define AURORA_FILE_NAME __FILE__
#endif

/// @brief 记录一条日志（自动附加 file:line）；消息支持任意数量的类型安全可变参数。
#define AURORA_LOG(level, category, ...)                                                                               \
    ::aurora::Logger::instance().log(AURORA_FILE_NAME, __LINE__, (level), (category),                                  \
                                     ::aurora::detail::log_concat(__VA_ARGS__))

/// @brief 记录 TRACE 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_TRACE(category, ...) AURORA_LOG(::aurora::LogLevel::Trace, (category), __VA_ARGS__)
/// @brief 记录 DEBUG 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_DEBUG(category, ...) AURORA_LOG(::aurora::LogLevel::Debug, (category), __VA_ARGS__)
/// @brief 记录 INFO 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_INFO(category, ...) AURORA_LOG(::aurora::LogLevel::Info, (category), __VA_ARGS__)
/// @brief 记录 WARN 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_WARN(category, ...) AURORA_LOG(::aurora::LogLevel::Warn, (category), __VA_ARGS__)
/// @brief 记录 ERROR 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_ERROR(category, ...) AURORA_LOG(::aurora::LogLevel::Error, (category), __VA_ARGS__)
/// @brief 记录 FATAL 级日志（category 后接任意数量的类型安全可变参数）。
#define AURORA_LOG_FATAL(category, ...) AURORA_LOG(::aurora::LogLevel::Fatal, (category), __VA_ARGS__)

/**
 * @brief 无前缀纯文本功能输出（经 `Logger::raw` 写入默认 stdout）。
 *
 * 不加时间戳/级别/分类前缀，调用方自负换行。用于 CLI 的 JSON 结果、usage 文本、
 * benchmark 表格、LSP/MCP 线协议帧等「程序产品」输出（区别于诊断日志 `AURORA_LOG_*`）。
 * @code
 *   AURORA_LOG_RAW("cli", json.dump(2), "\n");
 *   AURORA_LOG_RAW("mcp", "Content-Length: ", body.size(), "\r\n\r\n", body);
 * @endcode
 */
#define AURORA_LOG_RAW(category, ...)                                                                                  \
    ::aurora::Logger::instance().raw((category), ::aurora::detail::log_concat(__VA_ARGS__))

// ---------------------------------------------------------------------------
// printf 风格 → 日志桥接（宏形式，保留调用点的 file:line 归属；全局可用）
//
// 部分遗留代码（尤其测试）使用 printf 风格打印诊断信息。这里提供 `AURORA_TEST_PRINTF` /
// `AURORA_TEST_PRINTF_ERR`：先用 `std::snprintf` 写入**内存缓冲**（非标准输出），再经
// `Logger` 输出，从而把 printf 风格的诊断统一收口到日志接口，避免直接使用 stdout/stderr。
// 仅作 printf → 日志的兼容桥接，新代码请直接用 `AURORA_LOG_*` / `AURORA_LOG_RAW`。
#define AURORA_TEST_PRINTF(fmt, ...)                                                                                   \
    do {                                                                                                               \
        char _aurora_buf[2048];                                                                                        \
        std::snprintf(_aurora_buf, sizeof(_aurora_buf), fmt, ##__VA_ARGS__);                                           \
        std::string_view _aurora_sv{ _aurora_buf };                                                                    \
        while (!_aurora_sv.empty() && (_aurora_sv.back() == '\n' || _aurora_sv.back() == '\r'))                        \
            _aurora_sv.remove_suffix(1);                                                                               \
        AURORA_LOG_INFO("test", _aurora_sv);                                                                           \
    } while (0)

#define AURORA_TEST_PRINTF_ERR(fmt, ...)                                                                               \
    do {                                                                                                               \
        char _aurora_buf[2048];                                                                                        \
        std::snprintf(_aurora_buf, sizeof(_aurora_buf), fmt, ##__VA_ARGS__);                                           \
        std::string_view _aurora_sv{ _aurora_buf };                                                                    \
        while (!_aurora_sv.empty() && (_aurora_sv.back() == '\n' || _aurora_sv.back() == '\r'))                        \
            _aurora_sv.remove_suffix(1);                                                                               \
        AURORA_LOG_ERROR("test", _aurora_sv);                                                                          \
    } while (0)
