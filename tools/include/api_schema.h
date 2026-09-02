// ============================================================================
// api_schema.h — aurora_api.json「骨架」组装原语
// ----------------------------------------------------------------------------
// 由 gen_api_tools / aurora_cli / aurora_mcp 复用，避免 library/language/include/alias
// + widgets 枚举 + enums 段三份重复构建。widgets 枚举经由 WidgetRegistry + component_schema，
// 与 serialization::list_all_schemas() 逐字等价（见 src/aurora/widget/serialization.cpp），
// 故抽取后生成的 aurora_api.json 内容不变。
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
