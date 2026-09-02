// test_cli.cpp — aurora CLI 工具链测试（v0.8.0）。
//
// 测试策略：直接调用 CLI 消费的库 API，验证子命令逻辑正确性；
// 同时通过 std::system 运行 aurora_cli 验证端到端。
#include <cstdlib>
#include <string>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"

#include "test_harness.h"

using aurora::describe_component;
using aurora::Json;
using aurora::list_all_components;
using aurora::list_all_schemas;
using aurora::Node;
using aurora::search_components;
using aurora::serialization::CodeStyle;
using aurora::serialization::from_json;
using aurora::serialization::register_core_widgets;

// ---------- CLI 子命令消费的库 API 测试 ----------

static void test_cmd_components() {
    register_core_widgets();
    const auto types = list_all_components();
    AURORA_TEST_CHECK(!types.empty());

    // 验证 JSON 输出格式
    Json arr = Json::array();
    for (const auto &t : types) {
        arr.push_back(t);
    }
    const std::string output = arr.dump(2);
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
    AURORA_TEST_CHECK(output.find("Text") != std::string::npos);
}

static void test_cmd_describe() {
    Json schema = describe_component("Button");
    AURORA_TEST_CHECK(!schema.empty());
    AURORA_TEST_CHECK(schema["type"] == "Button");
    AURORA_TEST_CHECK(schema.contains("prop_descriptors"));

    // 未知组件
    Json unknown = describe_component("FooBar");
    AURORA_TEST_CHECK(!unknown.contains("prop_descriptors") || unknown["prop_descriptors"].empty());
}

static void test_cmd_search() {
    const auto results = search_components("but");
    AURORA_TEST_CHECK(!results.empty());

    Json arr = Json::array();
    for (const auto &r : results) {
        arr.push_back(r);
    }
    const std::string output = arr.dump();
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
}

static void test_cmd_validate() {
    // 合法树
    Json valid = Json::object();
    valid["type"] = "Text";
    valid["props"] = Json{ { "content", "hi" } };
    valid["children"] = Json::array();

    auto w = from_json(valid);
    AURORA_TEST_CHECK(w.ok());
    Node root(std::move(w.value()));
    auto ok = validate(root);
    AURORA_TEST_CHECK(ok.ok());

    // 非法树（空类型）
    Json invalid = Json::object();
    invalid["type"] = "";
    invalid["props"] = Json::object();
    invalid["children"] = Json::array();

    auto w2 = from_json(invalid);
    if (w2.ok()) {
        Node root2(std::move(w2.value()));
        auto ok2 = validate(root2);
        AURORA_TEST_CHECK(!ok2.ok());
    } else {
        AURORA_TEST_CHECK(true); // from_json 拒绝也算通过
    }
}

static void test_cmd_snapshot() {
    Json tree = Json::object();
    tree["type"] = "Column";
    tree["props"] = Json::object();
    Json children = Json::array();
    Json child = Json::object();
    child["type"] = "Text";
    child["props"] = Json{ { "content", "Hello" } };
    child["children"] = Json::array();
    children.push_back(child);
    tree["children"] = children;

    auto w = from_json(tree);
    AURORA_TEST_CHECK(w.ok());
    Node root(std::move(w.value()));
    Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK(snap["type"] == "Column");
    AURORA_TEST_CHECK(snap.contains("children"));
    AURORA_TEST_CHECK(snap["children"].is_array());
    AURORA_TEST_CHECK(!snap["children"].empty());
    AURORA_TEST_CHECK(snap["children"][0]["type"] == "Text");
}

static void test_cmd_to_code() {
    Json tree = Json::object();
    tree["type"] = "Button";
    tree["props"] = Json{ { "label", "Click" } };
    tree["children"] = Json::array();

    // Fluent
    const std::string code_f = to_code(tree, CodeStyle::Fluent);
    AURORA_TEST_CHECK(!code_f.empty());
    AURORA_TEST_CHECK(code_f.find("Button") != std::string::npos);

    // StepByStep
    const std::string code_s = to_code(tree, CodeStyle::StepByStep);
    AURORA_TEST_CHECK(!code_s.empty());

    // DesignatedInit
    const std::string code_d = to_code(tree, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK(!code_d.empty());
}

static void test_cmd_schema() {
    const auto schemas = list_all_schemas();
    AURORA_TEST_CHECK(!schemas.empty());

    Json api = Json::object();
    api["library"] = "aurora";
    api["language"] = "c++20";
    Json widgets = Json::array();
    for (const auto &s : schemas) {
        widgets.push_back(s);
    }
    api["widgets"] = widgets;

    const std::string output = api.dump();
    AURORA_TEST_CHECK(output.find("aurora") != std::string::npos);
    AURORA_TEST_CHECK(output.find("widgets") != std::string::npos);
    AURORA_TEST_CHECK(output.find("Button") != std::string::npos);
}

// ---------- CLI 端到端测试（运行 aurora_cli 可执行文件） ----------

// 跨平台子进程调用：拼接可执行文件路径（仓库根/build 目录两种 CWD）与 null 设备重定向。
static auto run_tool(const char *exe, const char *args) -> int {
#ifdef AURORA_PLATFORM_WINDOWS
    const std::string null_dev = "nul";
    const std::string primary = std::string{ "build\\" } + exe + ".exe " + args + " >" + null_dev + " 2>&1";
    const std::string fallback = std::string{ exe } + ".exe " + args + " >" + null_dev + " 2>&1";
#else
    const std::string null_dev = "/dev/null";
    const std::string primary = std::string{ "./" } + exe + " " + args + " >" + null_dev + " 2>&1";
    const std::string fallback = std::string{ "build/" } + exe + " " + args + " >" + null_dev + " 2>&1";
#endif
    // 测试用意拉起自构建二进制，输入非不可信。
    // NOLINTNEXTLINE(bugprone-command-processor)
    int ret = std::system(primary.c_str());
    if (ret != 0) {
        // 测试用意拉起自构建二进制，输入非不可信。
        // NOLINTNEXTLINE(bugprone-command-processor)
        ret = std::system(fallback.c_str());
    }
    return ret;
}

static void test_cli_e2e() {
    // 验证 --help 正常退出
    int ret = run_tool("aurora_cli", "--help");
    AURORA_TEST_CHECK(ret == 0);

    // 验证 components 子命令
    ret = run_tool("aurora_cli", "components");
    AURORA_TEST_CHECK(ret == 0);

    // 验证 schema 子命令
    ret = run_tool("aurora_cli", "schema");
    AURORA_TEST_CHECK(ret == 0);

    // 验证 describe 子命令
    ret = run_tool("aurora_cli", "describe Button");
    AURORA_TEST_CHECK(ret == 0);

    // preview 缺参数 → 用法错误 2（不触发窗口创建路径）
    ret = run_tool("aurora_cli", "preview");
    AURORA_TEST_CHECK(ret == 2);

    // 验证未知命令返回错误（两种 CWD 下均应非 0；run_tool 已先后尝试两个路径，
    // 若第一路径命中且返回非 0 会误试第二路径，故这里单独判断：任一路径返回非 0 即通过）。
    ret = run_tool("aurora_cli", "nonexistent");
    AURORA_TEST_CHECK(ret != 0);
}

AURORA_TEST() {
    test_cmd_components();
    test_cmd_describe();
    test_cmd_search();
    test_cmd_validate();
    test_cmd_snapshot();
    test_cmd_to_code();
    test_cmd_schema();
    test_cli_e2e();
}
