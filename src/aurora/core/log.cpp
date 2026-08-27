#include "aurora/core/log.h"

#include "aurora/core/platform.h"
#include "aurora/core/string_util.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <windows.h>
#endif

#include <mutex>
#include <sstream>
#include <thread>

namespace aurora {

// GCC/libstdc++ 对 std::call_once 内部 mutex 机身会产生 -Warray-bounds 误报
// （已知编译器缺陷，非真实越界）；此处局部抑制，避免阻断 -Werror 构建。
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

auto init_console() noexcept -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    static std::once_flag s_once;
    std::call_once(s_once, []() -> void {
        // 让控制台以 UTF-8 解释输出字节，修复中文乱码（默认 GBK/CP936）。
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
    });
#endif
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

auto Logger::instance() -> Logger & {
    static Logger s;
    init_console(); // 安全网：任何使用日志的消费者自动获得正确控制台代码页
    return s;
}

auto Logger::set_level(LogLevel level) noexcept -> void { m_level = level; }

auto Logger::level() const noexcept -> LogLevel { return m_level; }

auto Logger::set_sink(LogSink sink) -> void {
    if (sink) {
        m_sink = std::move(sink);
    } else {
        m_sink = default_sink();
    }
}

auto Logger::set_enabled(bool enabled) noexcept -> void { m_enabled = enabled; }

auto Logger::is_enabled() const noexcept -> bool { return m_enabled; }

auto Logger::log(std::string_view file, int line_no, LogLevel level, std::string_view category,
                 std::string_view message) const -> void {
    if (!m_enabled || level < m_level) {
        return;
    }

    // 当前线程 id，用于多线程序列化日志时区分来源。
    std::ostringstream tid_oss;
    tid_oss << std::this_thread::get_id();

    std::string out = "[" + log_timestamp() + "]" + "[" + std::string{ log_level_label(level) } + "]";
    out += '[';
    out += category.empty() ? std::string_view{ "-" } : category;
    out += '@';
    out += tid_oss.str();
    out += ' ';
    out += file.empty() ? std::string_view{ "-" } : file;
    out += ':';
    out += internal::string_format("%d", line_no);
    out += ']';
    out += " > ";
    out += message;
    out += '\n';

    if (m_sink) {
        m_sink(out);
    }
}

auto Logger::raw(std::string_view /*category*/, std::string_view message) const -> void {
    // 功能输出：始终打印（不受级别阈值 / m_enabled 影响），且不加任何前缀。
    if (m_raw_sink) {
        m_raw_sink(message);
    }
}

auto Logger::set_raw_sink(LogSink sink) -> void {
    if (sink) {
        m_raw_sink = std::move(sink);
    } else {
        m_raw_sink = default_raw_sink();
    }
}

auto Logger::default_sink() -> LogSink {
    return [](std::string_view l) -> void { std::fprintf(stderr, "%.*s", static_cast<int>(l.size()), l.data()); };
}

auto Logger::default_raw_sink() -> LogSink {
    return [](std::string_view l) -> void {
        std::fprintf(stdout, "%.*s", static_cast<int>(l.size()), l.data());
        std::fflush(stdout); // 保证 LSP/MCP 等 stdio 线协议逐条消息即时送达对端
    };
}

} // namespace aurora
