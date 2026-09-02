// test_api_json_integrity.cpp — aurora_api.json 完整性与漂移守卫。
//
// 背景（为何需要这个测试）：
//   cmake/AuroraTools.cmake 里 `generate_error_codes` 把 aurora_api.json 声明为自己的 OUTPUT，
//   且 `aurora` 库依赖该目标。因此当 aurora_api.json 缺失（被清理/误删）时，构建会由
//   gen_error_codes 把它**重建成只含 "error_codes" 一段的残缺文件**；而补齐其余 8 段的
//   gen_api_tools 挂在非默认目标 `aurora_api_json` 上，不会自动运行——残缺可长期无人察觉。
//   （2026-08-06 即发现该文件长期只剩 error_codes 一段，16KB。）
//
// 本测试守两件事：
//   1) 段完整性：9 个顶层段齐全且非空，标量段取值正确。
//   2) widget 漂移：JSON 中 widgets[].type 集合，必须与运行时
//      register_core_widgets() 后 WidgetRegistry::list_types() 完全一致。
//      新增/删除 widget 却忘了重新生成 JSON 时，此处立刻变红。
//
// 失败时的修复动作：
//   cmake --build build --target aurora_api_json
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/widget/serialization.h"

#include "known_enums.h"
#include "test_harness.h"

using aurora::Json;
using aurora::serialization::register_core_widgets;
using aurora::serialization::WidgetRegistry;

namespace fs = std::filesystem;

// ---- 辅助：定位仓库根的 aurora_api.json ----
// ctest 默认把 CWD 设为 build/，直接从仓库根运行可执行文件时 CWD 又是仓库根，故多候选探测。
static auto find_api_json() -> fs::path {
    const fs::path candidates[] = {
        "aurora_api.json",
        "../aurora_api.json",
        "../../aurora_api.json",
    };
    for (const auto &c : candidates) {
        if (fs::exists(c) && fs::is_regular_file(c)) {
            return c;
        }
    }
    return {};
}

static auto load_json(const fs::path &p) -> Json {
    const std::ifstream in(p, std::ios::binary);
    if (!in) {
        return Json{};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return Json::parse(ss.str());
    } catch (...) {
        return Json{};
    }
}

// ---- 校验 1：顶层段齐全 ----
static auto check_sections(const Json &api) -> void {
    // gen_api_tools 产出 8 段 + gen_error_codes 产出 error_codes 段 = 9 段。
    const char *required[] = { "library", "language",     "include",        "alias",      "widgets",
                               "enums",   "layout_rules", "state_patterns", "error_codes" };
    for (const char *key : required) {
        const bool present = api.contains(key);
        AURORA_TEST_CHECK_MSG(present, std::string("aurora_api.json contains top-level section: ") + key);
        if (!present) {
            continue;
        }
        const Json &v = api[key];
        // 标量段非空字符串；容器段非空。
        if (v.is_string()) {
            AURORA_TEST_CHECK_MSG(!v.get<std::string>().empty(), std::string("section non-empty: ") + key);
        } else {
            AURORA_TEST_CHECK_MSG(!v.empty(), std::string("section non-empty: ") + key);
        }
    }
}

// ---- 校验 2：标量段取值（与 tools/gen/gen_api.cpp 常量保持一致）----
static auto check_scalars(const Json &api) -> void {
    struct Pair {
        const char *key;
        const char *expect;
    };
    constexpr Pair pairs[] = {
        { .key = "library", .expect = "aurora" },
        { .key = "language", .expect = "c++20" },
        { .key = "include", .expect = "aurora/aurora.h" },
        { .key = "alias", .expect = "au" },
    };
    for (const auto &p : pairs) {
        if (!api.contains(p.key) || !api[p.key].is_string()) {
            AURORA_TEST_CHECK_MSG(false, std::string("scalar section readable: ") + p.key);
            continue;
        }
        const auto got = api[p.key].get<std::string>();
        AURORA_TEST_CHECK_MSG(got == p.expect, std::string("scalar section value ") + p.key + " == " + p.expect);
    }
}

// ---- 校验 3：widget 集合无漂移（核心）----
static auto check_widget_drift(const Json &api) -> void {
    if (!api.contains("widgets") || !api["widgets"].is_array()) {
        AURORA_TEST_CHECK_MSG(false, "widgets section exists and is an array");
        return;
    }

    std::set<std::string> in_json;
    for (const auto &w : api["widgets"]) {
        if (w.contains("type") && w["type"].is_string()) {
            in_json.insert(w["type"].get<std::string>());
        }
    }
    AURORA_TEST_CHECK_MSG(!in_json.empty(), "widgets section contains at least one entry with a type");

    register_core_widgets();
    const std::vector<std::string> types = WidgetRegistry::instance().list_types();
    const std::set in_registry(types.begin(), types.end());

    AURORA_TEST_CHECK_MSG(!in_registry.empty(), "WidgetRegistry registers at least one widget type");

    // 差集报告：便于一眼看出该补生成还是该改代码。
    std::vector<std::string> missing_in_json; // registry 有、JSON 无 -> 新增 widget 后忘了重生成
    std::vector<std::string> stale_in_json;   // JSON 有、registry 无 -> 删除 widget 后忘了重生成
    std::ranges::set_difference(in_registry, in_json, std::back_inserter(missing_in_json));
    std::ranges::set_difference(in_json, in_registry, std::back_inserter(stale_in_json));

    for (const auto &t : missing_in_json) {
        AURORA_LOG_ERROR("test", "  widget present in registry but missing from aurora_api.json: ", t);
    }
    for (const auto &t : stale_in_json) {
        AURORA_LOG_ERROR("test", "  widget stale in aurora_api.json but absent from registry: ", t);
    }

    AURORA_TEST_CHECK_MSG(
        missing_in_json.empty() && stale_in_json.empty(),
        "widgets set matches WidgetRegistry (if mismatch run: cmake --build build --target aurora_api_json)");
}

// ---- 校验 4：enums 段与已知枚举登记表（tools/include/known_enums.h）双向无漂移 ----
// gen_api.cpp 的 enums 段由 known_enums() 生成；若后者被改而忘了重生成 aurora_api.json，
// 或反之，此处立刻变红。比对类型名集合与每个类型的取值集合（双向）。
static auto check_enums_drift(const Json &api) -> void {
    if (!api.contains("enums") || !api["enums"].is_array()) {
        AURORA_TEST_CHECK_MSG(false, "enums section exists and is an array");
        return;
    }

    // 期望集合（SSOT）：known_enums.h
    const auto expected = aurora::tools::known_enums();

    // 实际集合：api["enums"] = [ {name, values:[...]}, ... ]
    std::map<std::string, std::set<std::string>> actual;
    for (const auto &e : api["enums"]) {
        if (e.contains("name") && e["name"].is_string() && e.contains("values") && e["values"].is_array()) {
            std::set<std::string> vals;
            for (const auto &v : e["values"]) {
                if (v.is_string()) {
                    vals.insert(v.get<std::string>());
                }
            }
            actual[e["name"].get<std::string>()] = std::move(vals);
        }
    }
    AURORA_TEST_CHECK_MSG(!actual.empty(), "enums section contains at least one record");

    // 类型名双向漂移
    std::vector<std::string> missing_in_json;
    std::vector<std::string> stale_in_json;
    for (const auto &name : expected | std::views::keys) {
        if (!actual.contains(name)) {
            missing_in_json.push_back(name);
        }
    }
    for (const auto &name : actual | std::views::keys) {
        if (!expected.contains(name)) {
            stale_in_json.push_back(name);
        }
    }
    for (const auto &n : missing_in_json) {
        AURORA_LOG_ERROR("test", "  enum present in known_enums but missing from aurora_api.json: ", n);
    }
    for (const auto &n : stale_in_json) {
        AURORA_LOG_ERROR("test", "  enum stale in aurora_api.json but absent from known_enums: ", n);
    }
    AURORA_TEST_CHECK_MSG(
        missing_in_json.empty() && stale_in_json.empty(),
        "enum type names match known_enums() (if mismatch run: cmake --build build --target aurora_api_json)");

    // 取值双向漂移（仅对共同类型名）
    int value_drift = 0;
    for (const auto &[name, vals] : expected) {
        auto it = actual.find(name);
        if (it == actual.end()) {
            continue;
        }
        std::vector<std::string> missing_vals;
        std::vector<std::string> stale_vals;
        for (const auto &v : vals) {
            if (!it->second.contains(v)) {
                missing_vals.push_back(v);
            }
        }
        for (const auto &v : it->second) {
            if (std::ranges::find(vals, v) == vals.end()) {
                stale_vals.push_back(v);
            }
        }
        for (const auto &v : missing_vals) {
            AURORA_LOG_ERROR("test", "  enum ", name, " missing value: ", v);
        }
        for (const auto &v : stale_vals) {
            AURORA_LOG_ERROR("test", "  enum ", name, " stale value: ", v);
        }
        value_drift += static_cast<int>(missing_vals.size() + stale_vals.size());
    }
    AURORA_TEST_CHECK_MSG(value_drift == 0, "enum values match known_enums() bidirectionally (if mismatch run: cmake "
                                            "--build build --target aurora_api_json)");
}

AURORA_TEST() {
    const fs::path api_path = find_api_json();
    if (api_path.empty()) {
        AURORA_TEST_CHECK_MSG(false, "located aurora_api.json at repository root");
    }
    AURORA_TEST_PRINTF("using: %s\n", api_path.string().c_str());

    const Json api = load_json(api_path);
    if (api.is_null() || !api.is_object()) {
        AURORA_TEST_CHECK_MSG(false, "aurora_api.json parses into a JSON object");
    }

    check_sections(api);
    check_scalars(api);
    check_widget_drift(api);
    check_enums_drift(api);
}
