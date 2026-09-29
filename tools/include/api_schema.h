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
#include <aurora/widget/props_io.h>

#include "known_enums.h"

namespace aurora::tools {

inline auto build_api_skeleton() -> Json {
    Json api = Json::object();
    api.set("library", "aurora");
    api.set("language", "c++20");
    api.set("include", "aurora/aurora.h");
    api.set("alias", "au");

    Json widgets = Json::array();
    for (const std::string &type : serialization::WidgetRegistry::instance().list_types()) {
        widgets.push_back(serialization::component_schema(type));
    }
    api.set("widgets", widgets);

    Json enums = Json::array();
    for (const auto &[name, vals] : known_enums()) {
        Json e = Json::object();
        e.set("name", name);
        Json v = Json::array();
        for (const auto &x : vals) {
            v.push_back(x);
        }
        e.set("values", v);
        enums.push_back(e);
    }
    api.set("enums", enums);

    return api;
}

}  // namespace aurora::tools
