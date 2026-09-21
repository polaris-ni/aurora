/// 测试类型: integration
/// 目标单元: include/aurora/app/validate_ui.h
/// 测试说明: AI 兼容性管线——JSON fixture 目录遍历（valid_*.json 必须通过
///           from_json + validate_ui；error_*.json 必须在管线某处被拒绝），
///           未知类型错误码、validate_ui_tree_json 报告格式、
///           from_json → to_json → to_code 完整管线往返

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef AURORA_BACKEND_HEADLESS
// TestController 的实现体同样以 AURORA_BACKEND_HEADLESS 门控（依赖 HeadlessSurface），
// 故宏关闭时不得引入声明，否则链接期缺符号。
#include "aurora/app/test_controller.h"
#endif

#include "aurora/app/validate_ui.h"
#include "aurora/aurora.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/serialization.h"
#include "framework/aurora_test.h"
#include "paths.h"

namespace aurora::test_cases::itest_ai_compat {

namespace fs = std::filesystem;

using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;
using au::serialization::to_json;

namespace {

// ---- fixture 目录：框架已把 cwd 切到仓库根，此处再经 under_repo 兜底绝对路径 ----
auto fixture_dir() -> fs::path { return au::testing::paths::under_repo("tests/fixtures/ai_compat"); }

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

// ===========================================================================
// 交互脚本 fixture（interact_*.json）——AI 兼容性管线的第二段：
//   「静态树 → TestController 交互 → 状态断言」回归脚本。
//
// 格式契约（tests/fixtures/ai_compat/interact_*.json）：
//   name      : 人类可读名（仅诊断用）
//   viewport  : {width, height}，缺省 800×600
//   tree      : 与 valid_*.json 同构的 widget 树（经 serialization::from_json 建树）
//   steps     : 有序动作数组，每项 {"action": ..., ...}
//                 settle               跑到 idle 帧（可选帧上限）
//                 pump {"frames": n}     推 n 帧，缺省 1
//                 tap {"target": sel}    点击目标
//                 drag {"target": sel, "dx": x, "dy": y}   从目标中心拖拽
//                 enter_text {"target": sel, "text": "..."}  目标输入文本
//   expect    : 断言数组，每项含 target 且为下列之一
//                 {"prop": k, "value": v}   属性值等于 v
//                 {"prop": k, "changed": true}  属性值相对脚本起点已变化
//                 {"visible": true}            目标可见（show 且几何非空）
//   target(sel): {"type": "Checkbox", "index": 0} 或 {"text": "..."}，index 缺省 0
//
// 目标用「类型 + 同类型序号」而非 key 的原因：`serialization::from_json` 只产出
// `shared_ptr<Widget>` 树，子节点 id 无法经 `Widget::child_nodes()`（const 引用）回填。
// ===========================================================================

#ifdef AURORA_BACKEND_HEADLESS

namespace {

/// @brief 脚本段的失败原因；空串表示该段通过。
using ScriptError = std::string;

/// @brief 目标定位：类型（+同类型序号）或文本命中。找不到返回空 Node。
[[nodiscard]] auto script_target(const au::TestController &tc, const au::Json &sel) -> au::Node {
    std::vector<au::Node> hits;
    if (sel.contains("type") && sel["type"].is_string()) {
        hits = tc.find_by_type(sel["type"].get<std::string>());
    } else if (sel.contains("text") && sel["text"].is_string()) {
        hits = tc.find_by_text(sel["text"].get<std::string>());
    } else {
        return au::Node{};
    }
    const std::size_t index =
        sel.contains("index") && sel["index"].is_number_unsigned() ? sel["index"].get<std::size_t>() : 0U;
    return index < hits.size() ? hits[index] : au::Node{};
}

/// @brief 读属性的 JSON 表示（缺失返回 null）。
[[nodiscard]] auto script_prop(const au::Node &n, const std::string &key) -> au::Json {
    au::Json props = au::Json::object();
    n.widget().serialize_props(props);
    return props.contains(key) ? props.at(key) : au::Json{};
}

/// @brief `changed` 断言的基线键（同一 props 组合唯一定位一条断言）。
[[nodiscard]] auto baseline_key(const au::Json &expectation) -> std::string {
    return expectation.value("target", au::Json::object()).dump() + "|" + expectation.value("prop", std::string{});
}

/// @brief 执行单个步骤。
[[nodiscard]] auto run_step(au::TestController &tc, const au::Json &step) -> ScriptError {
    const au::Json action_json = step.value("action", au::Json{});
    if (!action_json.is_string()) {
        return "step missing string 'action'";
    }
    const std::string action = action_json.get<std::string>();

    if (action == "settle") {
        // 脚本 settle 动作只要求驱动到稳定，返回帧数无消费方
        static_cast<void>(tc.pump_and_settle(step.value("max_frames", 60)));
        return ScriptError{};
    }
    if (action == "pump") {
        const au::Result<void> r = tc.pump(step.value("frames", 1));
        return r.ok() ? ScriptError{} : ScriptError{"pump: " + r.error().message};
    }

    const au::Node target = script_target(tc, step.value("target", au::Json::object()));
    if (!target) {
        return action + ": target not found";
    }
    if (action == "tap") {
        const au::Result<void> r = tc.tap(target);
        return r.ok() ? ScriptError{} : ScriptError{"tap: " + r.error().message};
    }
    if (action == "drag") {
        const au::Point delta{.x = step.value("dx", 0.0F), .y = step.value("dy", 0.0F)};
        const au::Result<void> r = tc.drag(target, delta);
        return r.ok() ? ScriptError{} : ScriptError{"drag: " + r.error().message};
    }
    if (action == "enter_text") {
        const std::string text = step.value("text", std::string{});
        const au::Result<void> r = tc.enter_text(target, text);
        return r.ok() ? ScriptError{} : ScriptError{"enter_text: " + r.error().message};
    }
    return "unknown action '" + action + "'";
}

/// @brief 建树 → 跑脚本 → 验断言；返回首个失败原因（空串表示全部通过）。
[[nodiscard]] auto run_interact_fixture(const au::Json &fx) -> ScriptError {
    const au::Json tree = fx.value("tree", au::Json{});
    if (!tree.is_object()) {
        return "fixture missing 'tree' object";
    }
    auto built = from_json(tree);
    if (!built.ok()) {
        return "from_json: " + built.error().message;
    }

    au::TestControllerConfig cfg{};
    if (fx.contains("viewport")) {
        cfg.width = fx["viewport"].value("width", cfg.width);
        cfg.height = fx["viewport"].value("height", cfg.height);
    }
    au::TestController tc{au::Node{std::move(built.value())}, cfg};

    // 首帧：布局与绘制确立几何；缺了这一步，后续 tap/drag 的命中测试没有可命中的框。
    if (const au::Result<void> r = tc.pump(); !r.ok()) {
        return "first frame: " + r.error().message;
    }

    const au::Json expectations = fx.value("expect", au::Json::array());
    if (!expectations.is_array() || expectations.empty()) {
        return "fixture must declare a non-empty 'expect' array";
    }

    // `changed` 断言的基线：脚本起点（首帧之后、任何交互之前）的属性快照。
    std::map<std::string, au::Json> baseline;
    for (const au::Json &e : expectations) {
        if (!e.value("changed", false)) {
            continue;
        }
        const au::Node target = script_target(tc, e.value("target", au::Json::object()));
        if (!target) {
            return "expect target not found";
        }
        baseline[baseline_key(e)] = script_prop(target, e.value("prop", std::string{}));
    }

    int step_index = 0;
    for (const au::Json &step : fx.value("steps", au::Json::array())) {
        const ScriptError err = run_step(tc, step);
        if (!err.empty()) {
            return "step[" + std::to_string(step_index) + "]: " + err;
        }
        ++step_index;
    }

    int expect_index = 0;
    for (const au::Json &e : expectations) {
        const au::Node target = script_target(tc, e.value("target", au::Json::object()));
        if (!target) {
            return "expect[" + std::to_string(expect_index) + "]: target not found";
        }
        if (e.contains("visible")) {
            const au::Result<void> r = aurora::TestController::expect_visible(target);
            if (!r.ok()) {
                return "expect[" + std::to_string(expect_index) + "]: " + r.error().message;
            }
        } else if (e.contains("prop")) {
            const std::string prop = e["prop"].get<std::string>();
            if (e.contains("value")) {
                const au::Result<void> r = aurora::TestController::expect_prop(target, prop, e["value"]);
                if (!r.ok()) {
                    return "expect[" + std::to_string(expect_index) + "]: " + r.error().message;
                }
            } else if (e.value("changed", false)) {
                const au::Json before = baseline.at(baseline_key(e));
                const au::Json after = script_prop(target, prop);
                if (before == after) {
                    return "expect[" + std::to_string(expect_index) + "]: prop '" + prop + "' unchanged (both " +
                           after.dump() + ")";
                }
            } else {
                return "expect[" + std::to_string(expect_index) + "]: needs 'value' or 'changed'";
            }
        } else {
            return "expect[" + std::to_string(expect_index) + "]: needs 'prop' or 'visible'";
        }
        ++expect_index;
    }
    return ScriptError{};
}

}  // namespace

AURORA_TEST_CASE(interact_fixtures_pass_testcontroller_scripts) {
    register_core_widgets();

    const std::filesystem::path dir = fixture_dir();
    AURORA_TEST_REQUIRE(std::filesystem::is_directory(dir));
    const auto files = collect_fixtures(dir, "interact_");
    AURORA_TEST_REQUIRE_MSG(!files.empty(), "at least one interact_*.json fixture must exist");

    for (const auto &p : files) {
        const std::string label = "interact fixture " + p.filename().string();
        const au::Json j = load_fixture(p);
        AURORA_TEST_REQUIRE_MSG(!j.is_null(), label + ": loaded");
        const ScriptError err = run_interact_fixture(j);
        AURORA_TEST_CHECK_MSG(err.empty(), label + ": " + err);  // NOLINT(*-inefficient-string-concatenation)
    }
}

AURORA_TEST_CASE(interact_fixtures_cover_at_least_three_scripts) {
    // 完成判据固化成用例：至少 3 个交互回归 fixture。
    const auto files = collect_fixtures(fixture_dir(), "interact_");
    AURORA_TEST_CHECK_MSG(files.size() >= 3U,
                          "interact fixtures count >= 3 (got " + std::to_string(files.size()) + ")");
}

AURORA_TEST_CASE(interact_script_reports_missing_target) {
    register_core_widgets();

    // 反面用例：目标选不到时脚本必须报错，而不是「零断言通过」地静默变绿。
    au::Json fx;
    fx["tree"] = au::Json{{"type", "Column"}, {"props", au::Json::object()}};
    fx["steps"] = au::Json::array({au::Json{{"action", "tap"}, {"target", au::Json{{"type", "NoSuchWidget"}}}}});
    fx["expect"] =
        au::Json::array({au::Json{{"target", au::Json{{"type", "NoSuchWidget"}}}, {"prop", "show"}, {"value", true}}});

    const ScriptError err = run_interact_fixture(fx);
    AURORA_TEST_CHECK_MSG(!err.empty(), "missing target must be reported");
    AURORA_TEST_CHECK_MSG(err.find("target not found") != std::string::npos,
                          "error must name the cause (got: " + err + ")");
}

#endif  // AURORA_BACKEND_HEADLESS

}  // namespace aurora::test_cases::itest_ai_compat
