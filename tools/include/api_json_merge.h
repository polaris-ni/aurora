// ============================================================================
// api_json_merge.h — aurora_api.json 的「段合并」原语（merge-only）
// ----------------------------------------------------------------------------
// 仅依赖标准库与 nlohmann/json，零 Aurora 依赖。由 gen_error_codes / gen_debug_api 复用，
// 避免「读取现有文件 → 若损坏则中止（绝不截断其它段）→ 写入指定段 → 整体写回」两份重复实现。
//
// 行为契约（与抽取前逐字一致）：
//   - 文件缺失 → 视为首次生成，从空对象起（无其它段可保护）；
//   - 文件解析失败（疑似损坏 / 截断）→ 报告错误并返回 false，绝不写空对象覆盖其它段；
//   - 解析结果非对象 → 报告错误并返回 false；
//   - 否则 doc[section] = value 后以 dump(2) 整体写回。
//
// report 为调用方的诊断收口函数（如各生成器的 err()），错误信息前缀由调用方决定。
// ============================================================================
#pragma once

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace aurora::tools {

inline auto merge_api_json_section(const std::string &path, const std::string &section, const nlohmann::json &value,
                                   void (*report)(const std::string &)) -> bool {
    nlohmann::json doc;
    {
        std::ifstream in(path);
        if (in) {
            try {
                in >> doc;
            } catch (...) {
                report("failed to parse " + path + " (suspected corrupt); aborting to avoid truncating other sections");
                return false;
            }
        } else {
            // 文件缺失：首次生成，从空对象起（无其它段可保护）。
            doc = nlohmann::json::object();
        }
    }
    if (!doc.is_object()) {
        report(path + " is not an object; aborting to avoid truncation");
        return false;
    }
    doc[section] = value; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            report("cannot write " + path);
            return false;
        }
        out << doc.dump(2) << "\n";
    }
    return true;
}

} // namespace aurora::tools
