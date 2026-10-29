#include "aurora/core/diagnostics.h"

#include "aurora/core/json.h"
#include "aurora/core/strict_mode.h"

namespace aurora {

auto Diagnostic::to_json_line() const -> std::string {
    auto j = json::Value::object();
    j.set("severity", std::string(to_string(severity)));
    j.set("domain", std::string(to_string(domain)));
    j.set("kind", std::string(to_string(kind)));
    j.set("message", message);
    if (!where.empty()) {
        j.set("where", where);
    }
    if (!code.empty()) {
        j.set("code", code);
    }
    const auto text = json::dump(j);
    return text ? std::move(text).value() : std::string{};
}

void Diagnostics::report(std::string_view message, std::string_view where, std::string_view code, bool is_degraded) {
    Diagnostic d;
    d.message = std::string(message);
    d.where = std::string(where);
    d.code = std::string(code);

    // 表驱动：code 为冻结 slug 时，从 g_error_table 注入 severity / domain / kind / code_enum。
    // 无 code 时退化为 General / Internal / Warning，并由 is_degraded 决定日志级别。
    if (!d.code.empty()) {
        for (const auto &m : AURORA_ERROR_TABLE) {
            if (m.slug == d.code) {
                d.severity = m.severity;
                d.domain = m.domain;
                d.kind = m.kind;
                d.code_enum = m.code;
                break;
            }
        }
    }
    if (d.code_enum == ErrorCode::GeneralUnknown) {
        d.severity = is_degraded ? ErrorSeverity::Error : ErrorSeverity::Warning;
        d.domain = ErrorDomain::General;
        d.kind = ErrorKind::Internal;
    }

    std::string line = d.to_json_line();
    const LogLevel lvl = (d.severity == ErrorSeverity::Error) ? LogLevel::Error : LogLevel::Warn;
    Logger::instance().log(AURORA_FILE_NAME, __LINE__, lvl, "diagnostics", line);

    Diagnostics &self = instance();
    self.log_.push_back(d);
    self.recent_.push_back(d);
    if (self.recent_.size() > AURORA_RECENT_CAP) {
        self.recent_.erase(self.recent_.begin());
    }
}

void Diagnostics::warn(std::string_view message, std::string_view where, std::string_view code) {
    report(message, where, code, false);
}

void Diagnostics::degraded(std::string_view message, std::string_view where, std::string_view code) {
    report(message, where, code, true);
    // 严格模式：降级即硬失败。不依赖 AURORA_ASSERT（NDEBUG 下会被剥离），
    // 改为 on_strict_failure 保证 Release/CI 构建也能被阻断（默认 std::terminate）。
    if (strict_mode() == StrictMode::On) {
        on_strict_failure("[strict] degraded diagnostic: " + std::string(code) + ": " + std::string(message));
    }
}

auto Diagnostics::take() -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out = std::move(instance().log_);
    instance().log_.clear();
    return out;
}

auto Diagnostics::count() -> std::size_t { return instance().log_.size(); }

auto Diagnostics::get_last_diagnostics() -> std::vector<Diagnostic> { return instance().recent_; }

auto Diagnostics::explain_diagnostic(std::string_view code) -> std::string {
    // 表驱动：诊断码即 errors.toml 的 slug，解释文本取自 ErrorMeta.hint（单一声明源）。
    for (const auto &m : AURORA_ERROR_TABLE) {
        if (m.slug == code) {
            return std::string(m.hint);
        }
    }
    return std::string("Unknown diagnostic code '") + std::string(code) +
           "'. Available codes are listed in codespec/ERROR_CATALOG.md.";
}

auto Diagnostics::explain_diagnostic(ErrorCode code) -> std::string { return explain_diagnostic(slug(code)); }

auto Diagnostics::instance() -> Diagnostics & {
    static Diagnostics inst;
    return inst;
}

}  // namespace aurora
