// ============================================================================
// api_json_merge.h — aurora_api.json "section merge" primitive (merge-only)
// ----------------------------------------------------------------------------
// Depends only on the standard library and nlohmann/json, zero Aurora dependencies. Reused by
// gen_error_codes / gen_debug_api to avoid two duplicate implementations of "read existing file →
// abort if corrupt (never truncate other sections) → write the target section → write the whole
// file back".
//
// Behavior contract (verbatim as before extraction):
//   - file missing → treat as first generation, start from an empty object (no other sections to protect);
//   - file parse failure (suspected corruption / truncation) → report an error and return false,
//     never write an empty object over other sections;
//   - parse result is not an object → report an error and return false;
//   - otherwise set doc[section] = value and write the whole document back with dump(2).
//
// report is the caller's diagnostic funnel (e.g. each generator's err()); the error message prefix
// is decided by the caller.
// ============================================================================
#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

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
            // file missing: first generation, start from an empty object (no other sections to protect).
            doc = nlohmann::json::object();
        }
    }
    if (!doc.is_object()) {
        report(path + " is not an object; aborting to avoid truncation");
        return false;
    }
    doc[section] = value;  // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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

}  // namespace aurora::tools
