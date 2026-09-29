// ============================================================================
// api_json_merge.h — aurora_api.json "section merge" primitive (merge-only)
// ----------------------------------------------------------------------------
// Depends only on the standard library and aurora core/json, zero Aurora widget dependencies.
// Reused by gen_error_codes / gen_debug_api to avoid two duplicate implementations of
// "read existing file → abort if corrupt (never truncate other sections) → write the target
// section → write the whole file back".
//
// Behavior contract (verbatim as before extraction):
//   - file missing → treat as first generation, start from an empty object (no other sections to protect);
//   - file parse failure (suspected corruption / truncation) → report an error and return false,
//     never write an empty object over other sections;
//   - parse result is not an object → report an error and return false;
//   - otherwise set doc[section] = value and write the whole document back with dump(indent=2).
//
// report is the caller's diagnostic funnel (e.g. each generator's err()); the error message prefix
// is decided by the caller.
// ============================================================================
#pragma once

#include <aurora/widget/props_io.h>

#include <fstream>
#include <sstream>
#include <string>

namespace aurora::tools {

inline auto merge_api_json_section(const std::string &path, const std::string &section, const Json &value,
                                   void (*report)(const std::string &)) -> bool {
    Json doc;
    {
        std::ifstream in(path);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            const auto parsed = json::parse(ss.str());
            if (!parsed) {
                report("failed to parse " + path + " (suspected corrupt); aborting to avoid truncating other sections");
                return false;
            }
            doc = std::move(parsed.value());
        } else {
            // file missing: first generation, start from an empty object (no other sections to protect).
            doc = Json::object();
        }
    }
    if (!doc.is_object()) {
        report(path + " is not an object; aborting to avoid truncation");
        return false;
    }
    doc.set(section, value);
    {
        const auto dumped = json::dump(doc, {.indent = 2});
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            report("cannot write " + path);
            return false;
        }
        out << (dumped.ok() ? dumped.value() : std::string{}) << "\n";
    }
    return true;
}

}  // namespace aurora::tools
