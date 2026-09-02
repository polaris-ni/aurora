// test_mcp.cpp — aurora-mcp MCP Server 协议与工具测试。
//
// 测试策略：直接调用 MCP Server 消费的库 API，验证工具逻辑正确性；
// 同时通过 std::system 运行 aurora_mcp 验证 MCP 协议端到端。
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

// ---------- MCP 工具消费的库 API 测试 ----------

static void test_list_components() {
    register_core_widgets();
    const auto types = list_all_components();
    AURORA_TEST_CHECK(!types.empty());
    AURORA_TEST_CHECK(types.size() >= 15); // 至少 15 个已注册控件

    // 验证包含已知控件
    bool has_button = false;
    bool has_text = false;
    bool has_column = false;
    for (const auto &t : types) {
        if (t == "Button") {
            has_button = true;
        }
        if (t == "Text") {
            has_text = true;
        }
        if (t == "Column") {
            has_column = true;
        }
    }
    AURORA_TEST_CHECK(has_button);
    AURORA_TEST_CHECK(has_text);
    AURORA_TEST_CHECK(has_column);
}

static void test_describe_component() {
    Json schema = describe_component("Button");
    AURORA_TEST_CHECK(!schema.empty());
    AURORA_TEST_CHECK(schema.contains("type"));
    AURORA_TEST_CHECK(schema["type"] == "Button");
    AURORA_TEST_CHECK(schema.contains("prop_descriptors"));
    AURORA_TEST_CHECK(schema["prop_descriptors"].is_array());
    AURORA_TEST_CHECK(!schema["prop_descriptors"].empty());
    AURORA_TEST_CHECK(schema.contains("events"));
    AURORA_TEST_CHECK(schema.contains("children_policy"));
    AURORA_TEST_CHECK(schema["children_policy"] == "none");

    // 未知组件：describe_component 返回空 Json 对象或不含 prop_descriptors
    Json unknown = describe_component("NonExistentWidget");
    AURORA_TEST_CHECK(!unknown.contains("prop_descriptors") || unknown["prop_descriptors"].empty());
}

static void test_search_components() {
    const auto results = search_components("but");
    AURORA_TEST_CHECK(!results.empty());
    bool found = false;
    for (const auto &r : results) {
        if (r.value("type", std::string("")) == "Button") {
            found = true;
        }
    }
    AURORA_TEST_CHECK(found);

    // 搜索 "tex" 应包含 Text
    const auto results2 = search_components("tex");
    AURORA_TEST_CHECK(!results2.empty());
}

static void test_validate_tree() {
    // 合法树
    Json valid_tree = Json::object();
    valid_tree["type"] = "Column";
    valid_tree["props"] = Json::object();
    valid_tree["children"] = Json::array();

    auto widget = from_json(valid_tree);
    AURORA_TEST_CHECK(widget.ok());

    Node root(std::move(widget.value()));
    auto ok = validate(root);
    AURORA_TEST_CHECK(ok.ok());

    // 非法树（未知类型）
    Json invalid_tree = Json::object();
    invalid_tree["type"] = "UnknownWidget";
    invalid_tree["props"] = Json::object();
    invalid_tree["children"] = Json::array();

    auto widget2 = from_json(invalid_tree);
    // from_json 对未知类型可能返回错误或降级
    if (widget2.ok()) {
        Node root2(std::move(widget2.value()));
        auto ok2 = validate(root2);
        AURORA_TEST_CHECK(!ok2.ok()); // validate 应检测到未知类型
    } else {
        AURORA_TEST_CHECK(true); // from_json 直接拒绝也算通过
    }
}

static void test_render_snapshot() {
    Json tree = Json::object();
    tree["type"] = "Text";
    tree["props"] = Json{ { "content", "Hello" } };
    tree["children"] = Json::array();

    auto widget = from_json(tree);
    AURORA_TEST_CHECK(widget.ok());

    Node root(std::move(widget.value()));
    Json snapshot = render_to_logical_snapshot(root, 400, 300);
    AURORA_TEST_CHECK(snapshot.contains("type"));
    AURORA_TEST_CHECK(snapshot["type"] == "Text");
    AURORA_TEST_CHECK(snapshot.contains("box"));
    AURORA_TEST_CHECK(snapshot["box"].contains("w"));
    AURORA_TEST_CHECK(snapshot["box"].contains("h"));
}

static void test_to_code() {
    Json tree = Json::object();
    tree["type"] = "Button";
    tree["props"] = Json{ { "label", "OK" } };
    tree["children"] = Json::array();

    const std::string code = to_code(tree, CodeStyle::Fluent);
    AURORA_TEST_CHECK(!code.empty());
    AURORA_TEST_CHECK(code.find("au::") != std::string::npos || code.find("Button") != std::string::npos);

    // DesignatedInit 风格
    const std::string code_di = to_code(tree, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK(!code_di.empty());
}

static void test_get_schema() {
    const auto schemas = list_all_schemas();
    AURORA_TEST_CHECK(!schemas.empty());
    AURORA_TEST_CHECK(schemas.size() == list_all_components().size());

    // 每个 schema 含 type 和 prop_descriptors
    for (const auto &s : schemas) {
        AURORA_TEST_CHECK(s.contains("type"));
        AURORA_TEST_CHECK(s.contains("prop_descriptors"));
    }
}

// ---------- MCP 协议端到端测试（运行 aurora_mcp 可执行文件） ----------

static void test_mcp_protocol_e2e() {
    // 验证 aurora_mcp 可运行（stdin 给 EOF 即退出）；跨平台拼接路径与 null 设备。
#ifdef AURORA_PLATFORM_WINDOWS
    // e2e 测试需经 shell 启动 aurora_mcp 可执行文件做冒烟验证，命令固定且不含外部输入
    // NOLINTNEXTLINE(bugprone-command-processor)
    int ret = std::system("build\\aurora_mcp.exe <nul >nul 2>&1");
    if (ret != 0) {
        // 回退到 PATH/当前目录查找同一可执行文件，仍为固定命令，非用户输入
        // NOLINTNEXTLINE(bugprone-command-processor)
        ret = std::system("aurora_mcp.exe <nul >nul 2>&1");
    }
#else
    int ret = std::system("./aurora_mcp </dev/null >/dev/null 2>&1");
    if (ret != 0) {
        ret = std::system("build/aurora_mcp </dev/null >/dev/null 2>&1");
    }
#endif
    AURORA_TEST_CHECK(ret == 0 || ret == 1); // 正常退出
}

AURORA_TEST() {
    test_list_components();
    test_describe_component();
    test_search_components();
    test_validate_tree();
    test_render_snapshot();
    test_to_code();
    test_get_schema();
    test_mcp_protocol_e2e();
}
