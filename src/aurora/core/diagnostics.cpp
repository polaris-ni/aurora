#include "aurora/core/diagnostics.h"

#include <algorithm>
#include <unordered_map>

#include "aurora/core/strict_mode.h"
#include "aurora/widget/props_io.h" // Json（to_json_line 使用 nlohmann::json）

namespace aurora {

auto Diagnostic::to_json_line() const -> std::string {
    Json j = Json::object();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["severity"] = std::string(to_string(severity));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["category"] = std::string(to_string(category));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["message"] = message;
    if (!where.empty()) {
        j["where"] = where; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (!code.empty()) {
        j["code"] = code; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (fix && fix->has_auto_fix()) {
        j["fix_code"] = fix->code;        // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["fix_desc"] = fix->description; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j.dump();
}

void Diagnostics::report(std::string_view message, std::string_view where, std::string_view code, bool is_degraded,
                         FixSuggestion fix) {
    Diagnostic d;
    d.message = std::string(message);
    d.where = std::string(where);
    d.code = std::string(code);

    // 表驱动：code 为冻结 slug 时，从 g_error_table 注入 severity / category / code_enum。
    // 无 code 时退化为 General / Warning，并由 is_degraded 决定日志级别。
    if (!d.code.empty()) {
        for (const auto &m : AURORA_ERROR_TABLE) {
            if (m.slug == d.code) {
                d.severity = m.severity;
                d.category = m.category;
                d.code_enum = m.code;
                break;
            }
        }
    }
    if (d.code_enum == ErrorCode::GeneralUnknown) {
        d.severity = is_degraded ? ErrorSeverity::Error : ErrorSeverity::Warning;
        d.category = ErrorCategory::General;
    }

    if (fix.has_auto_fix() || !fix.code.empty() || !fix.description.empty()) {
        d.fix = std::move(fix);
    }

    std::string line = d.to_json_line();
    const LogLevel lvl = (d.severity == ErrorSeverity::Error) ? LogLevel::Error : LogLevel::Warn;
    Logger::instance().log(AURORA_FILE_NAME, __LINE__, lvl, "diagnostics", line);

    Diagnostics &self = instance();
    self.m_log.push_back(d);
    self.m_recent.push_back(d);
    if (self.m_recent.size() > AURORA_RECENT_CAP) {
        self.m_recent.erase(self.m_recent.begin());
    }
}

void Diagnostics::warn(std::string_view message, std::string_view where, std::string_view code) {
    report(message, where, code, false, FixSuggestion{});
}

void Diagnostics::degraded(std::string_view message, std::string_view where, std::string_view code) {
    report(message, where, code, true, FixSuggestion{});
    // 严格模式：降级即硬失败。不依赖 AURORA_ASSERT（NDEBUG 下会被剥离），
    // 改为 on_strict_failure 保证 Release/CI 构建也能被阻断（默认 std::terminate）。
    if (strict_mode() == StrictMode::On) {
        on_strict_failure("[strict] degraded diagnostic: " + std::string(code) + ": " + std::string(message));
    }
}

auto Diagnostics::take() -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out = std::move(instance().m_log);
    instance().m_log.clear();
    return out;
}

auto Diagnostics::count() -> std::size_t { return instance().m_log.size(); }

auto Diagnostics::get_last_diagnostics() -> std::vector<Diagnostic> { return instance().m_recent; }

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

auto Diagnostics::collect_fixes() -> std::vector<FixSuggestion> {
    std::vector<FixSuggestion> out;
    for (const auto &d : instance().m_recent) {
        if (d.fix && (!d.fix->code.empty() || d.fix->has_auto_fix())) {
            out.push_back(*d.fix);
        }
    }
    return out;
}

auto Diagnostics::apply_fix(std::string_view code) -> bool {
    const std::string key{ code };
    const auto it = std::ranges::find_if(instance().m_recent, [&](const Diagnostic &d) -> bool {
        return d.fix && d.fix->code == key && d.fix->has_auto_fix();
    });
    if (it != instance().m_recent.end() && it->fix) {
        it->fix->auto_fix();
        return true;
    }
    return false;
}

// ---- 需求 #9：错误码 → 修复策略注册表 ----

namespace {
/// @brief 全局错误码 → 修复策略注册表（main-thread only，同 Diagnostics 线程模型）。
auto fix_registry() -> auto & {
    static std::unordered_map<ErrorCode, FixSuggestion> registry;
    return registry;
}
} // namespace

auto Diagnostics::register_fix(ErrorCode code, FixSuggestion fix) -> void { fix_registry()[code] = std::move(fix); }

auto Diagnostics::auto_fix_all() -> std::size_t {
    std::size_t count = 0;
    for (auto &fix : fix_registry() | std::views::values) {
        if (fix.has_auto_fix()) {
            fix.auto_fix();
            ++count;
        }
    }
    return count;
}

auto Diagnostics::apply_fix(ErrorCode code) -> bool {
    auto &reg = fix_registry();
    const auto it = reg.find(code);
    if (it != reg.end() && it->second.has_auto_fix()) {
        it->second.auto_fix();
        return true;
    }
    return false;
}

} // namespace aurora
