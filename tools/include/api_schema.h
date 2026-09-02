// ============================================================================
// api_schema.h — aurora_api.json "skeleton" assembly primitive
// ----------------------------------------------------------------------------
// Reused by gen_api_tools / aurora_cli / aurora_mcp to avoid three duplicate constructions of
// library/language/include/alias + the widgets enumeration + the enums section. The widgets
// enumeration goes through WidgetRegistry + component_schema and is verbatim equivalent to
// serialization::list_all_schemas() (see src/aurora/widget/serialization.cpp), so the generated
// aurora_api.json content is unchanged after extraction.
// ============================================================================
#pragma once

#include <aurora/aurora.h>
#include <nlohmann/json.hpp>

#include "known_enums.h"

namespace aurora::tools {

inline auto build_api_skeleton() -> nlohmann::json {
    nlohmann::json api = nlohmann::json::object();
    api["library"] = "aurora";
    api["language"] = "c++20";
    api["include"] = "aurora/aurora.h";
    api["alias"] = "au";

    nlohmann::json widgets = nlohmann::json::array();
    for (const std::string &type : serialization::WidgetRegistry::instance().list_types()) {
        widgets.push_back(serialization::component_schema(type));
    }
    api["widgets"] = widgets;

    nlohmann::json enums = nlohmann::json::array();
    for (const auto &[name, vals] : known_enums()) {
        nlohmann::json e = nlohmann::json::object();
        e["name"] = name;
        nlohmann::json v = nlohmann::json::array();
        for (const auto &x : vals) {
            v.push_back(x);
        }
        e["values"] = v;
        enums.push_back(e);
    }
    api["enums"] = enums;

    return api;
}

} // namespace aurora::tools
