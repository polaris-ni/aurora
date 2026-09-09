/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: 读仓库根 aurora_api.json 验证段完整性与标量值，并把 widgets/enums 两段
///           与运行时 WidgetRegistry、tools/include/known_enums.h 双向比对，锁死生成物漂移
/// 覆盖说明: 对应 CTest 门禁 check_api_schema_sync 的 C++ 层对照；漂移时修复动作
///           为 cmake --build build --target aurora_api_json

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "known_enums.h"
#include "paths.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_api_json_integrity {

using au::serialization::register_core_widgets;
using au::serialization::WidgetRegistry;

namespace {

// ---- 辅助：加载仓库根 aurora_api.json（缺失/坏 JSON 时致命终止当前用例）----
auto load_api_json() -> au::Json {
    const std::string path = au::testing::paths::under_repo("aurora_api.json");
    std::ifstream in(path, std::ios::binary);
    AURORA_TEST_REQUIRE_MSG(in.good(), "aurora_api.json must exist at repository root: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    au::Json api = au::Json{};
    try {
        api = au::Json::parse(ss.str());
    } catch (...) {
        api = au::Json{};
    }
    AURORA_TEST_REQUIRE_MSG(api.is_object(), "aurora_api.json must parse into a JSON object: " + path);
    return api;
}

// 把 JSON 段整理为 {widget类型集合} / {枚举名 -> 取值集合}。
auto widget_types_in_json(const au::Json &api) -> std::set<std::string> {
    std::set<std::string> out;
    if (!api.contains("widgets") || !api["widgets"].is_array()) {
        return out;
    }
    for (const auto &w : api["widgets"]) {
        if (w.contains("type") && w["type"].is_string()) {
            out.insert(w["type"].get<std::string>());
        }
    }
    return out;
}

auto enums_in_json(const au::Json &api) -> std::map<std::string, std::set<std::string>> {
    std::map<std::string, std::set<std::string>> out;
    if (!api.contains("enums") || !api["enums"].is_array()) {
        return out;
    }
    for (const auto &e : api["enums"]) {
        if (!e.contains("name") || !e["name"].is_string() || !e.contains("values") || !e["values"].is_array()) {
            continue;
        }
        std::set<std::string> vals;
        for (const auto &v : e["values"]) {
            if (v.is_string()) {
                vals.insert(v.get<std::string>());
            }
        }
        out[e["name"].get<std::string>()] = std::move(vals);
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(api_json_contains_all_toolchain_sections) {
    const au::Json api = load_api_json();
    // gen_api_tools 产出 9 段 + gen_error_codes 产出 error_codes 段。
    const char *required[] = {"library", "language",      "include",   "alias",     "widgets",
                              "enums",   "layout_rules",  "state_patterns", "debug", "error_codes"};
    for (const char *key : required) {
        const bool present = api.contains(key);
        AURORA_TEST_CHECK_MSG(present, std::string("aurora_api.json contains top-level section: ") + key);
        if (!present) {
            continue;
        }
        const au::Json &v = api[key];
        // 标量段非空字符串；容器段非空。
        if (v.is_string()) {
            AURORA_TEST_CHECK_MSG(!v.get<std::string>().empty(), std::string("section non-empty: ") + key);
        } else {
            AURORA_TEST_CHECK_MSG(!v.empty(), std::string("section non-empty: ") + key);
        }
    }
}

AURORA_TEST_CASE(api_json_scalar_sections_hold_expected_values) {
    const au::Json api = load_api_json();
    const std::pair<const char *, const char *> pairs[] = {
        {"library", "aurora"},
        {"language", "c++20"},
        {"include", "aurora/aurora.h"},
        {"alias", "au"},
    };
    for (const auto &p : pairs) {
        if (!api.contains(p.first) || !api[p.first].is_string()) {
            AURORA_TEST_CHECK_MSG(false, std::string("scalar section readable: ") + p.first);
            continue;
        }
        const auto got = api[p.first].get<std::string>();
        AURORA_TEST_CHECK_MSG(got == p.second,
                              std::string("scalar section value ") + p.first + " == " + p.second + ", got " + got);
    }
}

AURORA_TEST_CASE(api_json_widget_set_matches_widget_registry) {
    const au::Json api = load_api_json();

    const std::set<std::string> in_json = widget_types_in_json(api);
    AURORA_TEST_REQUIRE_MSG(!in_json.empty(), "widgets section contains at least one entry with a type");

    register_core_widgets();
    const std::vector<std::string> types = WidgetRegistry::instance().list_types();
    const std::set<std::string> in_registry(types.begin(), types.end());
    AURORA_TEST_REQUIRE_MSG(!in_registry.empty(), "WidgetRegistry registers at least one widget type");

    // 差集报告：registry 有、JSON 无 → 新增 widget 后忘了重生成；反之 → 删除后忘了重生成。
    std::vector<std::string> missing_in_json;
    std::vector<std::string> stale_in_json;
    std::ranges::set_difference(in_registry, in_json, std::back_inserter(missing_in_json));
    std::ranges::set_difference(in_json, in_registry, std::back_inserter(stale_in_json));
    for (const auto &t : missing_in_json) {
        AURORA_TEST_TRACE("widget present in registry but missing from aurora_api.json: " + t);
    }
    for (const auto &t : stale_in_json) {
        AURORA_TEST_TRACE("widget stale in aurora_api.json but absent from registry: " + t);
    }
    AURORA_TEST_CHECK_MSG(missing_in_json.empty() && stale_in_json.empty(),
                          "widgets set matches WidgetRegistry (if mismatch run: cmake --build build --target "
                          "aurora_api_json)");
}

AURORA_TEST_CASE(api_json_enums_match_known_enums_registry) {
    const au::Json api = load_api_json();

    const std::map<std::string, std::set<std::string>> actual = enums_in_json(api);
    AURORA_TEST_REQUIRE_MSG(!actual.empty(), "enums section contains at least one record");

    // 期望集合（SSOT）：tools/include/known_enums.h
    const auto expected = aurora::tools::known_enums();

    // 类型名双向漂移。
    int drift = 0;
    for (const auto &[name, vals] : expected) {
        if (!actual.contains(name)) {
            AURORA_TEST_TRACE("enum present in known_enums but missing from aurora_api.json: " + name);
            ++drift;
        }
    }
    for (const auto &[name, vals] : actual) {
        if (!expected.contains(name)) {
            AURORA_TEST_TRACE("enum stale in aurora_api.json but absent from known_enums: " + name);
            ++drift;
        }
    }
    AURORA_TEST_CHECK_MSG(drift == 0,
                          "enum type names match known_enums() (if mismatch run: cmake --build build --target "
                          "aurora_api_json)");

    // 取值双向漂移（仅对共同类型名）。
    int value_drift = 0;
    for (const auto &[name, vals] : expected) {
        const auto it = actual.find(name);
        if (it == actual.end()) {
            continue;
        }
        for (const auto &v : vals) {
            if (!it->second.contains(v)) {
                AURORA_TEST_TRACE("enum " + name + " missing value: " + v);
                ++value_drift;
            }
        }
        for (const auto &v : it->second) {
            if (std::ranges::find(vals, v) == vals.end()) {
                AURORA_TEST_TRACE("enum " + name + " stale value: " + v);
                ++value_drift;
            }
        }
    }
    AURORA_TEST_CHECK_MSG(value_drift == 0,
                          "enum values match known_enums() bidirectionally (if mismatch run: cmake --build build "
                          "--target aurora_api_json)");
}

}  // namespace aurora::test_cases::itest_api_json_integrity
