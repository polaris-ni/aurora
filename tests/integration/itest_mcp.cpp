/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: aurora-mcp 工具链测试——库 API（list_all_components/describe_component/
///           search_components/from_json+validate/render_to_logical_snapshot/to_code/
///           list_all_schemas）直测 MCP 工具消费的逻辑；协议端到端按构建产物探测，
///           未构建则 SKIP
/// 覆盖说明: aurora_mcp 是独立可执行（tools/servers/），e2e 仅验证进程可运行

#include <cstdlib>
#include <filesystem>
#include <string>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "framework/aurora_test.h"
#include "paths.h"

namespace aurora::test_cases::itest_mcp {

using au::serialization::CodeStyle;
using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;

namespace {

// MCP 端到端：stdin 给 EOF 即退出（固定冒烟命令拉起自构建二进制，输出重定向 null 设备）。
auto run_mcp_smoke() -> int {
    std::string exe;
    for (const char* name : {"aurora_mcp.exe", "aurora_mcp"}) {
        const std::string p = au::testing::paths::under_repo(std::string{"build/"} + name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            exe = p;
            break;
        }
    }
    if (exe.empty()) {
        return -1;  // 未构建
    }
#ifdef AURORA_PLATFORM_WINDOWS
    const char* null_dev = "<nul >nul 2>&1";
#else
    const char* null_dev = "</dev/null >/dev/null 2>&1";
#endif
    const std::string cmd = "\"" + exe + "\" " + null_dev;
    // 测试用意拉起自构建二进制，命令固定且不含外部输入。
    // NOLINTNEXTLINE(bugprone-command-processor)
    return std::system(cmd.c_str());
}

}  // namespace

// ---------- MCP 工具消费的库 API 测试 ----------

AURORA_TEST_CASE(mcp_list_components_covers_core_widgets) {
    register_core_widgets();
    const auto types = au::list_all_components();
    AURORA_TEST_CHECK(!types.empty());
    AURORA_TEST_CHECK(types.size() >= 15);  // 至少 15 个已注册控件

    bool has_button = false;
    bool has_text = false;
    bool has_column = false;
    for (const auto& t : types) {
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

AURORA_TEST_CASE(mcp_describe_component_returns_full_schema) {
    const au::Json schema = au::describe_component("Button");
    AURORA_TEST_CHECK(!schema.empty());
    AURORA_TEST_CHECK(schema.contains("type"));
    AURORA_TEST_CHECK(schema["type"] == "Button");
    AURORA_TEST_CHECK(schema.contains("prop_descriptors"));
    AURORA_TEST_CHECK(schema["prop_descriptors"].is_array());
    AURORA_TEST_CHECK(!schema["prop_descriptors"].empty());
    AURORA_TEST_CHECK(schema.contains("events"));
    AURORA_TEST_CHECK(schema.contains("children_policy"));
    AURORA_TEST_CHECK(schema["children_policy"] == "none");

    // 未知组件：返回空 Json 对象或不含有效 prop_descriptors。
    const au::Json unknown = au::describe_component("NonExistentWidget");
    AURORA_TEST_CHECK(!unknown.contains("prop_descriptors") || unknown["prop_descriptors"].empty());
}

AURORA_TEST_CASE(mcp_search_components_finds_by_substring) {
    const auto results = au::search_components("but");
    AURORA_TEST_CHECK(!results.empty());
    bool found = false;
    for (const auto& r : results) {
        if (r.value("type", std::string{}) == "Button") {
            found = true;
        }
    }
    AURORA_TEST_CHECK(found);

    // 搜索 "tex" 应包含 Text。
    const auto results2 = au::search_components("tex");
    AURORA_TEST_CHECK(!results2.empty());
}

AURORA_TEST_CASE(mcp_validate_tree_detects_unknown_types) {
    // 合法树。
    au::Json valid_tree = au::Json::object();
    valid_tree["type"] = "Column";
    valid_tree["props"] = au::Json::object();
    valid_tree["children"] = au::Json::array();

    auto widget = from_json(valid_tree);
    AURORA_TEST_CHECK(widget.ok());
    if (widget.ok()) {
        au::Node root(std::move(widget.value()));
        const auto ok = au::validate(root);
        AURORA_TEST_CHECK(ok.ok());
    }

    // 非法树（未知类型）：from_json 对未知类型可能返回错误或降级。
    au::Json invalid_tree = au::Json::object();
    invalid_tree["type"] = "UnknownWidget";
    invalid_tree["props"] = au::Json::object();
    invalid_tree["children"] = au::Json::array();

    auto widget2 = from_json(invalid_tree);
    if (widget2.ok()) {
        au::Node root2(std::move(widget2.value()));
        const auto ok2 = au::validate(root2);
        AURORA_TEST_CHECK(!ok2.ok());  // validate 应检测到未知类型
    } else {
        AURORA_TEST_CHECK(true);  // from_json 直接拒绝也算通过
    }
}

AURORA_TEST_CASE(mcp_render_snapshot_reports_box_geometry) {
    au::Json tree = au::Json::object();
    tree["type"] = "Text";
    tree["props"] = au::Json{{"content", "Hello"}};
    tree["children"] = au::Json::array();

    auto widget = from_json(tree);
    AURORA_TEST_CHECK(widget.ok());
    if (!widget.ok()) {
        return;
    }
    au::Node root(std::move(widget.value()));
    const au::Json snapshot = au::render_to_logical_snapshot(root, 400, 300);
    AURORA_TEST_CHECK(snapshot.contains("type"));
    AURORA_TEST_CHECK(snapshot["type"] == "Text");
    AURORA_TEST_CHECK(snapshot.contains("box"));
    AURORA_TEST_CHECK(snapshot["box"].contains("w"));
    AURORA_TEST_CHECK(snapshot["box"].contains("h"));
}

AURORA_TEST_CASE(mcp_to_code_generates_fluent_and_designated_init) {
    au::Json tree = au::Json::object();
    tree["type"] = "Button";
    tree["props"] = au::Json{{"label", "OK"}};
    tree["children"] = au::Json::array();

    const std::string code = to_code(tree, CodeStyle::Fluent);
    AURORA_TEST_CHECK(!code.empty());
    AURORA_TEST_CHECK(code.find("au::") != std::string::npos || code.find("Button") != std::string::npos);

    const std::string code_di = to_code(tree, CodeStyle::DesignatedInit);
    AURORA_TEST_CHECK(!code_di.empty());
}

AURORA_TEST_CASE(mcp_get_schema_matches_component_count) {
    const auto schemas = au::list_all_schemas();
    AURORA_TEST_CHECK(!schemas.empty());
    AURORA_TEST_CHECK(schemas.size() == au::list_all_components().size());

    // 每个 schema 含 type 和 prop_descriptors。
    for (const auto& s : schemas) {
        AURORA_TEST_CHECK(s.contains("type"));
        AURORA_TEST_CHECK(s.contains("prop_descriptors"));
    }
}

// ---------- MCP 协议端到端测试（运行 aurora_mcp 可执行文件） ----------

AURORA_TEST_CASE(mcp_protocol_e2e_binary_smoke) {
    const int ret = run_mcp_smoke();
    if (ret == -1) {
        AURORA_TEST_SKIP("aurora_mcp 未构建");
    }
    AURORA_TEST_CHECK(ret == 0 || ret == 1);  // stdin EOF 后正常退出
}

}  // namespace aurora::test_cases::itest_mcp
