/// 测试类型: integration
/// 目标单元: include/aurora/app/validate_ui.h
/// 测试说明: AI 兼容性管线——JSON fixture 目录遍历（valid_*.json 必须通过
///           from_json + validate_ui；error_*.json 必须在管线某处被拒绝），
///           未知类型错误码、validate_ui_tree_json 报告格式、
///           from_json → to_json → to_code 完整管线往返

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/app/validate_ui.h"
#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/serialization.h"
#include "paths.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_ai_compat {

namespace fs = std::filesystem;

using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;
using au::serialization::to_json;

namespace {

// ---- fixture 目录：框架已把 cwd 切到仓库根，此处再经 under_repo 兜底绝对路径 ----
auto fixture_dir() -> fs::path {
    return au::testing::paths::under_repo("tests/fixtures/ai_compat");
}

// ---- 按文件名前缀收集 fixture（目录遍历驱动，无硬编码名单；新增 fixture 免改测试）----
auto collect_fixtures(const fs::path &dir, std::string_view prefix) -> std::vector<fs::path> {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto &ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".json") {
            continue;
        }
        const std::string name = ent.path().filename().string();
        if (name.starts_with(prefix)) {
            found.push_back(ent.path());
        }
    }
    return found;
}

// ---- 加载 JSON fixture 文件；读失败 / 解析失败返回 null ----
auto load_fixture(const fs::path &path) -> au::Json {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return au::Json{};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        return au::Json::parse(ss.str());
    } catch (...) {
        return au::Json{};
    }
}

}  // namespace

AURORA_TEST_CASE(valid_fixtures_pass_full_pipeline) {
    register_core_widgets();

    const auto dir = fixture_dir();
    AURORA_TEST_REQUIRE(std::filesystem::is_directory(dir));
    const auto valid_files = collect_fixtures(dir, "valid_");
    AURORA_TEST_REQUIRE_MSG(!valid_files.empty(), "at least one valid_*.json fixture must exist");

    for (const auto &p : valid_files) {
        const std::string label = "valid fixture " + p.filename().string();
        const au::Json j = load_fixture(p);
        AURORA_TEST_REQUIRE_MSG(!j.is_null(), label + ": loaded");
        const auto result = from_json(j);
        AURORA_TEST_CHECK_MSG(result.ok(), label + ": from_json ok");
        const auto errors = au::validate_ui_tree(j);
        AURORA_TEST_CHECK_MSG(errors.empty(), label + ": validate ok");
    }
}

AURORA_TEST_CASE(error_fixtures_rejected_by_pipeline) {
    register_core_widgets();

    const auto dir = fixture_dir();
    AURORA_TEST_REQUIRE(std::filesystem::is_directory(dir));
    const auto error_files = collect_fixtures(dir, "error_");
    AURORA_TEST_REQUIRE_MSG(!error_files.empty(), "at least one error_*.json fixture must exist");

    for (const auto &p : error_files) {
        const std::string label = "error fixture " + p.filename().string();
        const au::Json j = load_fixture(p);
        AURORA_TEST_REQUIRE_MSG(!j.is_null(), label + ": loaded");
        // 拒绝点二选一：from_json 失败，或 validate_ui_tree 报 error。
        const auto result = from_json(j);
        const auto errors = au::validate_ui_tree(j);
        const bool rejected = !result.ok() || !errors.empty();
        AURORA_TEST_CHECK_MSG(rejected, label + ": rejected by pipeline");
    }
}

AURORA_TEST_CASE(from_json_unknown_type_fails_with_code) {
    register_core_widgets();

    au::Json j;
    j["type"] = "BogusWidget";
    j["props"] = au::Json::object();

    const auto result = from_json(j);
    AURORA_TEST_CHECK_MSG(!result.ok(), "from_json unknown type: fails");
    AURORA_TEST_CHECK_MSG(!result.ok() && result.error().code == "widget-unknown-type",
                          "from_json unknown type: correct error code");
}

AURORA_TEST_CASE(validate_report_json_format) {
    register_core_widgets();

    au::Json good;
    good["type"] = "Text";
    good["props"]["content"] = "Hello";

    const au::Json report = au::validate_ui_tree_json(good);
    AURORA_TEST_CHECK(report["valid"].get<bool>());
    AURORA_TEST_CHECK(report["errors"].is_array());
    AURORA_TEST_CHECK(report["errors"].empty());

    au::Json bad;
    bad["type"] = "NoSuchWidget";
    const au::Json report2 = au::validate_ui_tree_json(bad);
    AURORA_TEST_CHECK(!report2["valid"].get<bool>());
    AURORA_TEST_CHECK(!report2["errors"].empty());
}

AURORA_TEST_CASE(full_pipeline_roundtrip_to_code) {
    register_core_widgets();

    // 构造一棵简单树：Column > Text
    au::Json j;
    j["type"] = "Column";
    j["props"] = au::Json::object();
    au::Json child;
    child["type"] = "Text";
    child["props"]["content"] = "Pipeline Test";
    j["children"] = au::Json::array({child});

    // 1. validate
    const auto errors = au::validate_ui_tree(j);
    AURORA_TEST_CHECK_MSG(errors.empty(), "pipeline: valid tree passes validate");

    // 2. from_json
    const auto w = from_json(j);
    AURORA_TEST_REQUIRE_MSG(w.ok(), "pipeline: from_json succeeds");

    // 3. to_json（往返保持类型）
    const au::Json j2 = to_json(*w.value());
    AURORA_TEST_CHECK(j2["type"] == "Column");

    // 4. to_code（生成代码提及 Column 且非空）
    const std::string code = to_code(j2);
    AURORA_TEST_CHECK_MSG(!code.empty(), "pipeline: to_code produces output");
    AURORA_TEST_CHECK(code.find("Column") != std::string::npos);
}

}  // namespace aurora::test_cases::itest_ai_compat
