#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "reporter.h"

namespace aurora::testing {

namespace {

/// @brief 把毫秒渲染为定宽小数字（报告里的 time 字段口径统一）。
[[nodiscard]] auto seconds(double elapsed_ms) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (elapsed_ms / 1000.0);
    return out.str();
}

[[nodiscard]] auto milliseconds(double elapsed_ms) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << elapsed_ms;
    return out.str();
}

/// @brief 用例全名 `Suite.Case` 的套件段。
[[nodiscard]] auto suite_of(std::string_view full_name) -> std::string {
    const auto dot = full_name.find('.');
    return std::string{dot == std::string_view::npos ? full_name : full_name.substr(0, dot)};
}

/// @brief 用例全名 `Suite.Case` 的用例段。
[[nodiscard]] auto case_of(std::string_view full_name) -> std::string {
    const auto dot = full_name.find('.');
    return dot == std::string_view::npos ? std::string{full_name} : std::string{full_name.substr(dot + 1)};
}

[[nodiscard]] auto status_text(TestStatus status) -> std::string_view {
    switch (status) {
        case TestStatus::Passed:
            return "passed";
        case TestStatus::Failed:
            return "failed";
        case TestStatus::Skipped:
            return "skipped";
    }
    return "unknown";
}

/// @brief XML 属性值里的换行/制表符须压平（属性不支持裸换行）。
[[nodiscard]] auto flatten(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    return out;
}

/// @brief 按套件分组（保持首次出现顺序），供 JUnit 的 testsuite 层级使用。
struct SuiteGroup {
    std::string suite;
    std::vector<std::size_t> indices;
    int failures = 0;
    int skipped = 0;
    double elapsed_ms = 0.0;
};

[[nodiscard]] auto group_by_suite(const std::vector<CaseResult>& results) -> std::vector<SuiteGroup> {
    std::vector<SuiteGroup> groups;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        const auto name = suite_of(result.full_name);
        auto group = std::ranges::find(groups, name, &SuiteGroup::suite);
        if (group == groups.end()) {
            groups.push_back({.suite = name});
            group = std::prev(groups.end());
        }
        group->indices.push_back(index);
        group->elapsed_ms += result.elapsed_ms;
        if (result.status == TestStatus::Failed) {
            ++group->failures;
        } else if (result.status == TestStatus::Skipped) {
            ++group->skipped;
        }
    }
    return groups;
}

auto write_xml(std::ostream& out, const std::vector<CaseResult>& results, const RunSummary& summary) -> void {
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << R"(<testsuites name="aurora" tests=")" << summary.total << "\" failures=\"" << summary.failed
        << "\" skipped=\"" << summary.skipped << "\" time=\"" << seconds(summary.elapsed_ms) << "\">\n";
    for (const auto& group : group_by_suite(results)) {
        out << "  <testsuite name=\"" << xml_escape(group.suite) << "\" tests=\"" << group.indices.size()
            << "\" failures=\"" << group.failures << "\" skipped=\"" << group.skipped << "\" time=\""
            << seconds(group.elapsed_ms) << "\">\n";
        for (const auto index : group.indices) {
            const auto& result = results[index];
            out << "    <testcase classname=\"" << xml_escape(group.suite) << "\" name=\""
                << xml_escape(case_of(result.full_name)) << "\" time=\"" << seconds(result.elapsed_ms) << "\"";
            if (result.status == TestStatus::Passed) {
                out << "/>\n";
                continue;
            }
            out << ">\n";
            if (result.status == TestStatus::Skipped) {
                out << "      <skipped message=\"" << xml_escape(flatten(result.skip_reason)) << "\"/>\n";
            }
            for (const auto& failure : result.failures) {
                const auto location = failure.file + ':' + std::to_string(failure.line);
                out << "      <failure message=\"" << xml_escape(flatten(failure.message))
                    << R"(" type="assertion" file=")" << xml_escape(failure.file) << "\" line=\"" << failure.line
                    << '">' << xml_escape(location + ": " + failure.message) << "</failure>\n";
            }
            out << "    </testcase>\n";
        }
        out << "  </testsuite>\n";
    }
    out << "</testsuites>\n";
}

auto write_json(std::ostream& out, const std::vector<CaseResult>& results, const RunSummary& summary) -> void {
    out << "{\n";
    out << "  \"name\": \"aurora\",\n";
    out << "  \"total\": " << summary.total << ",\n";
    out << "  \"passed\": " << summary.passed << ",\n";
    out << "  \"failed\": " << summary.failed << ",\n";
    out << "  \"skipped\": " << summary.skipped << ",\n";
    out << "  \"time_ms\": " << milliseconds(summary.elapsed_ms) << ",\n";
    out << "  \"cases\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        out << "    {\n";
        out << R"(      "name": ")" << json_escape(result.full_name) << "\",\n";
        out << R"(      "status": ")" << status_text(result.status) << "\",\n";
        out << "      \"time_ms\": " << milliseconds(result.elapsed_ms) << ",\n";
        out << R"(      "file": ")" << json_escape(result.file) << "\",\n";
        out << "      \"line\": " << result.line << ",\n";
        if (result.status == TestStatus::Skipped) {
            out << R"(      "skip_reason": ")" << json_escape(result.skip_reason) << "\",\n";
        }
        out << "      \"failures\": [";
        for (std::size_t slot = 0; slot < result.failures.size(); ++slot) {
            const auto& failure = result.failures[slot];
            out << (slot == 0 ? "\n" : ",\n") << R"(        {"file": ")" << json_escape(failure.file)
                << R"(", "line": )" << failure.line << R"(, "message": ")" << json_escape(failure.message) << "\"}";
        }
        out << (result.failures.empty() ? "],\n" : "\n      ],\n");
        out << "      \"notes\": [";
        for (std::size_t slot = 0; slot < result.notes.size(); ++slot) {
            out << (slot == 0 ? "\n" : ",\n") << "        \"" << json_escape(result.notes[slot]) << "\"";
        }
        out << (result.notes.empty() ? "]\n" : "\n      ]\n");
        out << "    }" << (index + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

}  // namespace

auto xml_escape(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                // XML 1.0 不允许的控制字符统一丢弃（保留 \t \n \r，属性侧由 flatten 处理）。
                out += (static_cast<unsigned char>(c) < 0x20U && c != '\t' && c != '\n' && c != '\r') ? '?' : c;
        }
    }
    return out;
}

auto json_escape(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(static_cast<unsigned char>(c));
                    out += escaped.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

auto write_report(std::string_view path, const std::vector<CaseResult>& results, const RunSummary& summary,
                  std::string* error) -> bool {
    const std::filesystem::path target{path};
    const auto suffix = target.extension().string();
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    if (!out) {
        if (error != nullptr) {
            *error = "cannot open report file: " + target.string();
        }
        return false;
    }
    if (suffix == ".xml") {
        write_xml(out, results, summary);
    } else {
        write_json(out, results, summary);
    }
    out.flush();
    if (!out.good()) {
        if (error != nullptr) {
            *error = "failed writing report file: " + target.string();
        }
        return false;
    }
    return true;
}

}  // namespace aurora::testing
