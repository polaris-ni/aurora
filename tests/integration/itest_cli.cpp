/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: aurora_cli 工具链测试——库 API（list_all_components/describe_component/
///           search_components/from_json+validate/render_to_logical_snapshot/to_code/
///           list_all_schemas）直测为主；子进程端到端冒烟按构建产物探测，未构建则 SKIP
/// 覆盖说明: aurora_cli 是独立可执行（tools/servers/），e2e 仅验证退出码契约

#include <cstdlib>
#include <filesystem>
#include <string>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "paths.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_cli {

using au::serialization::CodeStyle;
using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;

namespace {

// ---------- 端到端辅助：探测构建产物并拉起子进程 ----------

auto probe_cli_exe() -> std::string {
    for (const char *name : {"aurora_cli.exe", "aurora_cli"}) {
        const std::string p = au::testing::paths::under_repo(std::string{"build/"} + name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            return p;
        }
    }
    return {};
}

// 固定冒烟命令拉起自构建二进制，输出重定向到 null 设备（输入非不可信）。
auto run_cli(const std::string &exe, const char *args) -> int {
#ifdef AURORA_PLATFORM_WINDOWS
    const char *null_dev = ">nul 2>&1";
#else
    const char *null_dev = ">/dev/null 2>&1";
#endif
    const std::string cmd = "\"" + exe + "\" " + args + " " + null_dev;
    // 测试用意拉起自构建二进制，命令固定且不含外部输入。
    // NOLINTNEXTLINE(bugprone-command-processor)
    return std::system(cmd.c_str());
}

}  // namespace

// ---------- CLI 子命令消费的库 API 测试 ----------

AURORA_TEST_CASE(cli_components_lists_registered_widgets) {
    register_core_widgets();
    const auto types = au::list_all_components();
    AURORA_TEST_CHECK(!types.empty());

    // 验证 JSON 输出格式（CLI components 子命令按 JSON 数组打印类型名）。
    au::Json arr = au::Json::array();
    for (const auto &t : types) {
        arr.push_back(t);
    }
    const std::string output = arr.dump(2);
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
    AURORA_TEST_CHECK(output.find("Text") != std::string::npos);
}

AURORA_TEST_CASE(cli_describe_returns_button_schema) {
    const au::Json schema = au::describe_component("Button");
    AURORA_TEST_CHECK(!schema.empty());
    AURORA_TEST_CHECK(schema["type"] == "Button");
    AURORA_TEST_CHECK(schema.contains("prop_descriptors"));

    // 未知组件：返回空对象或不含有效 prop_descriptors。
    const au::Json unknown = au::describe_component("FooBar");
    AURORA_TEST_CHECK(!unknown.contains("prop_descriptors") || unknown["prop_descriptors"].empty());
}

AURORA_TEST_CASE(cli_search_finds_button_by_substring) {
    const auto results = au::search_components("but");
    AURORA_TEST_CHECK(!results.empty());

    au::Json arr = au::Json::array();
    for (const auto &r : results) {
        arr.push_back(r);
    }
    const std::string output = arr.dump();
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
}

AURORA_TEST_CASE(cli_validate_rejects_invalid_trees) {
    // 合法树。
    au::Json valid = au::Json::object();
    valid["type"] = "Text";
    valid["props"] = au::Json{{"content", "hi"}};
    valid["children"] = au::Json::array();

    auto w = from_json(valid);
    AURORA_TEST_CHECK(w.ok());
    if (w.ok()) {
        au::Node root(std::move(w.value()));
        const auto ok = au::validate(root);
        AURORA_TEST_CHECK(ok.ok());
    }

    // 非法树（空类型）：from_json 拒绝或 validate 报错均算通过。
    au::Json invalid = au::Json::object();
    invalid["type"] = "";
    invalid["props"] = au::Json::object();
    invalid["children"] = au::Json::array();

    auto w2 = from_json(invalid);
    if (w2.ok()) {
        au::Node root2(std::move(w2.value()));
        const auto ok2 = au::validate(root2);
        AURORA_TEST_CHECK(!ok2.ok());
    } else {
        AURORA_TEST_CHECK(true);
    }
}

AURORA_TEST_CASE(cli_snapshot_renders_logical_tree) {
    au::Json tree = au::Json::object();
    tree["type"] = "Column";
    tree["props"] = au::Json::object();
    au::Json child = au::Json::object();
    child["type"] = "Text";
    child["props"] = au::Json{{"content", "Hello"}};
    child["children"] = au::Json::array();
    tree["children"] = au::Json::array({child});

    auto w = from_json(tree);
    AURORA_TEST_CHECK(w.ok());
    if (!w.ok()) {
        return;
    }
    au::Node root(std::move(w.value()));
    const au::Json snap = au::render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK(snap["type"] == "Column");
    AURORA_TEST_CHECK(snap.contains("children"));
    AURORA_TEST_CHECK(snap["children"].is_array());
    AURORA_TEST_CHECK(!snap["children"].empty());
    AURORA_TEST_CHECK(snap["children"][0]["type"] == "Text");
}

AURORA_TEST_CASE(cli_to_code_supports_all_styles) {
    au::Json tree = au::Json::object();
    tree["type"] = "Button";
    tree["props"] = au::Json{{"label", "Click"}};
    tree["children"] = au::Json::array();

    const std::string code_f = to_code(tree, CodeStyle::Fluent);
    AURORA_TEST_CHECK(!code_f.empty());
    AURORA_TEST_CHECK(code_f.find("Button") != std::string::npos);

    const std::string code_s = to_code(tree, CodeStyle::StepByStep);
    AURORA_TEST_CHECK(!code_s.empty());

    const std::string code_d = to_code(tree, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK(!code_d.empty());
}

AURORA_TEST_CASE(cli_schema_lists_all_components_with_descriptors) {
    const auto schemas = au::list_all_schemas();
    AURORA_TEST_CHECK(!schemas.empty());
    AURORA_TEST_CHECK(schemas.size() == au::list_all_components().size());

    au::Json api = au::Json::object();
    api["library"] = "aurora";
    api["language"] = "c++20";
    au::Json widgets = au::Json::array();
    for (const auto &s : schemas) {
        AURORA_TEST_CHECK(s.contains("type"));
        AURORA_TEST_CHECK(s.contains("prop_descriptors"));
        widgets.push_back(s);
    }
    api["widgets"] = widgets;

    const std::string output = api.dump();
    AURORA_TEST_CHECK(output.find("aurora") != std::string::npos);
    AURORA_TEST_CHECK(output.find("widgets") != std::string::npos);
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
}

// ---------- CLI 端到端测试（运行 aurora_cli 可执行文件） ----------

AURORA_TEST_CASE(cli_e2e_binary_smoke) {
    const std::string exe = probe_cli_exe();
    if (exe.empty()) {
        AURORA_TEST_SKIP("aurora_cli 未构建");
    }

    // --help / components / schema / describe 正常退出。
    AURORA_TEST_CHECK(run_cli(exe, "--help") == 0);
    AURORA_TEST_CHECK(run_cli(exe, "components") == 0);
    AURORA_TEST_CHECK(run_cli(exe, "schema") == 0);
    AURORA_TEST_CHECK(run_cli(exe, "describe Button") == 0);

    // preview 缺参数 → 用法错误 2（不触发窗口创建路径）。
    AURORA_TEST_CHECK(run_cli(exe, "preview") == 2);

    // 未知命令返回非 0。
    AURORA_TEST_CHECK(run_cli(exe, "nonexistent") != 0);
}

}  // namespace aurora::test_cases::itest_cli
