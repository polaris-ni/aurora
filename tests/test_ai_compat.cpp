// test_ai_compat.cpp — AI 兼容性测试：验证 JSON fixture → from_json → validate_ui → to_code 管线。
// 覆盖：合法树反序列化成功、非法树验证失败、边界情况（空树、超深嵌套等）。
//
// 采用「目录遍历」驱动（无硬编码 fixture 名单）：fixtures 目录下所有 `valid_*.json` 期望
// 通过 from_json + validate_ui；所有 `error_*.json` 期望在管线某处被拒绝（from_json 失败
// 或 validate_ui 报 error）。新增 fixture 只需丢进目录即可纳入校验，无需改测试代码。
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/app/validate_ui.h"
#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/serialization.h"
#include "test_harness.h"

using aurora::Json;
using aurora::validate_ui_tree;
using aurora::validate_ui_tree_json;
using aurora::serialization::from_json;
using aurora::serialization::register_core_widgets;
using aurora::serialization::to_code;
using aurora::serialization::to_json;

namespace fs = std::filesystem;

// ---- 辅助：从多个候选路径中定位 fixture 目录 ----
static auto find_fixture_dir() -> fs::path {
    constexpr std::array candidates = {
        "tests/fixtures/ai_compat",
        "../tests/fixtures/ai_compat",
        "../../tests/fixtures/ai_compat",
    };
    for (const auto &c : candidates) {
        if (fs::exists(c) && fs::is_directory(c)) {
            return c;
        }
    }
    return {};
}

// ---- 辅助：加载 JSON fixture 文件（传入完整路径）----
static auto load_fixture(const fs::path &path) -> Json {
    const std::ifstream in(path, std::ios::binary);
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

// ---- 测试：from_json 对未知类型报错 ----
static void test_from_json_unknown_type() {
    Json j;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["type"] = "BogusWidget";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["props"] = Json::object();

    const auto result = from_json(j);
    AURORA_TEST_CHECK_MSG(!result.ok(), "from_json unknown type: fails");
    AURORA_TEST_CHECK_MSG(result.error().code == "widget-unknown-type", "from_json unknown type: correct error code");
}

// ---- 测试：validate_ui_tree_json 返回正确 JSON 报告 ----
static void test_validate_report_format() {
    Json j;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["type"] = "Text";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["props"]["content"] = "Hello";

    Json report = validate_ui_tree_json(j);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(report["valid"].get<bool>(), "validate report: valid tree reports valid=true");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(report["errors"].is_array(), "validate report: errors is array");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(report["errors"].empty(), "validate report: no errors for valid tree");

    Json bad;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    bad["type"] = "NoSuchWidget";
    Json report2 = validate_ui_tree_json(bad);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(!report2["valid"].get<bool>(), "validate report: invalid tree reports valid=false");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(!report2["errors"].empty(), "validate report: has errors for invalid tree");
}

// ---- 测试：完整管线 from_json → to_json → to_code 往返 ----
static void test_full_pipeline_roundtrip() {
    // 构造一棵简单树
    Json j;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["type"] = "Column";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["props"] = Json::object();
    Json child;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    child["type"] = "Text";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    child["props"]["content"] = "Pipeline Test";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    j["children"] = Json::array({child});

    // 1. validate
    auto errors = validate_ui_tree(j);
    AURORA_TEST_CHECK_MSG(errors.empty(), "pipeline: valid tree passes validate");

    // 2. from_json
    auto w = from_json(j);
    AURORA_TEST_CHECK_MSG(w.ok(), "pipeline: from_json succeeds");
    if (!w.ok()) {
        return;
    }

    // 3. to_json (round-trip)
    Json j2 = to_json(*w.value());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j2["type"] == "Column", "pipeline: round-trip preserves type");

    // 4. to_code
    std::string code = to_code(j2);
    AURORA_TEST_CHECK_MSG(!code.empty(), "pipeline: to_code produces output");
    AURORA_TEST_CHECK_MSG(code.find("Column") != std::string::npos, "pipeline: code mentions Column");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== ai_compat_test ===\n");

    // 确保核心 widget 已注册
    register_core_widgets();

    // 定位 fixture 目录
    auto fixdir = find_fixture_dir();
    if (fixdir.empty()) {
        AURORA_TEST_PRINTF("WARNING: fixture directory not found, skipping file-based tests\n");
    } else {
        AURORA_TEST_PRINTF("fixture dir: %s\n", fixdir.string().c_str());

        // 目录遍历：按文件名前缀分类，无硬编码名单。
        std::vector<fs::path> valid_files;
        std::vector<fs::path> error_files;
        for (const auto &ent : fs::directory_iterator(fixdir)) {
            if (!ent.is_regular_file()) {
                continue;
            }
            const auto &p = ent.path();
            if (p.extension() != ".json") {
                continue;
            }
            const std::string name = p.filename().string();
            if (name.starts_with("valid_")) {
                valid_files.push_back(p);
            } else if (name.starts_with("error_")) {
                error_files.push_back(p);
            }
            // 其它命名（如 manifest/README）忽略
        }

        // 合法 fixture：必须 from_json 成功且 validate 通过。
        int valid_count = 0;
        for (const auto &p : valid_files) {
            const std::string label = "valid fixture " + p.filename().string();
            Json j = load_fixture(p);
            AURORA_TEST_CHECK_MSG(!j.is_null(), label + ": loaded");
            if (j.is_null()) {
                continue;
            }
            auto result = from_json(j);
            AURORA_TEST_CHECK_MSG(result.ok(), label + ": from_json ok");
            auto errors = validate_ui_tree(j);
            AURORA_TEST_CHECK_MSG(errors.empty(), label + ": validate ok");
            ++valid_count;
        }
        AURORA_TEST_CHECK_MSG(valid_count > 0, "at least one valid_*.json fixture traversed");

        // 错误 fixture：必须在管线某处被拒绝（from_json 失败 或 validate 报 error）。
        int error_count = 0;
        for (const auto &p : error_files) {
            const std::string label = "error fixture " + p.filename().string();
            Json j = load_fixture(p);
            AURORA_TEST_CHECK_MSG(!j.is_null(), label + ": loaded");
            if (j.is_null()) {
                continue;
            }
            auto result = from_json(j);
            auto errors = validate_ui_tree(j);
            const bool rejected = (!result.ok()) || !errors.empty();
            AURORA_TEST_CHECK_MSG(rejected, label + ": rejected by pipeline");
            ++error_count;
        }
        AURORA_TEST_CHECK_MSG(error_count > 0, "at least one error_*.json fixture traversed");
    }

    // 不依赖文件的纯内存测试
    test_from_json_unknown_type();
    test_validate_report_format();
    test_full_pipeline_roundtrip();
}