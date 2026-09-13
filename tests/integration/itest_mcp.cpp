/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: aurora-mcp 工具链测试——库 API（list_all_components/describe_component/
///           search_components/from_json+validate/render_to_logical_snapshot/to_code/
///           list_all_schemas/simulate_*）直测 MCP 工具消费的逻辑；协议端到端按构建产物
///           探测，未构建则 SKIP
/// 覆盖说明: aurora_mcp 是独立可执行（tools/servers/），e2e 仅验证进程可运行

#include <cstdlib>
#include <filesystem>
#include <string>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/inspector/inspector_api.h"
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

// ---------- simulate_interaction 工具消费的库 API 测试 ----------
//
// 工具本体在 tools/servers/aurora_mcp.cpp（独立可执行，测试进程内无法直调），故此处按
// 其实现顺序调用同一组库 API：JSON 构树 → 派发前布局 → 按路径定位 → 合成事件 → 读回
// 目标属性与交互后快照。接线若偏离实现（例如漏掉派发前的布局），本组用例会失败。

namespace {

/// @brief 一次模拟的观测结果：目标属性快照 / 交互后逻辑快照 / 布局后的树 / 失败原因。
struct Simulation {
    au::Json props = au::Json::object();
    au::Json snapshot = au::Json::object();
    au::Node root;  // 交互发生后仍可继续导航，用于检查目标之外的控件
    std::string error;
};

[[nodiscard]] auto node_json(const std::string& type, au::Json props = au::Json::object(),
                             au::Json children = au::Json::array()) -> au::Json {
    au::Json n = au::Json::object();
    n["type"] = type;
    n["props"] = std::move(props);
    n["children"] = std::move(children);
    return n;
}

/// @brief 复刻 `simulate_interaction` 的消费路径。
[[nodiscard]] auto simulate(const au::Json& tree, const std::string& action, const std::string& path,
                            float dx = 0.0F, float dy = 0.0F, const std::string& text = {}) -> Simulation {
    constexpr int kWidth = 800;
    constexpr int kHeight = 600;
    Simulation out;
    auto widget = from_json(tree);
    if (!widget) {
        out.error = widget.error().message;
        return out;
    }
    au::Node root(std::move(widget.value()));
    // from_json 只构树不布局，而 simulate_* 以目标「中心点」为指针位置：未布局时控件尺寸为零、
    // 中心退化为自身原点，落点就不再是目标的真实中心。故与工具一致，先布局一次确立几何；
    // 其产物仅用于几何，不回传（回传的是交互之后的那份）。
    (void)au::render_to_logical_snapshot(root, kWidth, kHeight);
    out.root = root;

    au::Node target = au::Inspector::find_node(root, path);
    if (!target) {
        out.error = "widget not found at path '" + path + "'";
        return out;
    }

    std::string failure;
    if (action == "click") {
        const aurora::Result<void> r = au::Inspector::simulate_click(target.widget());
        failure = r ? std::string{} : r.error().message;
    } else if (action == "scroll") {
        const aurora::Result<void> r = au::Inspector::simulate_scroll(target.widget(), dx, dy);
        failure = r ? std::string{} : r.error().message;
    } else if (action == "text") {
        const aurora::Result<void> r = au::Inspector::simulate_text_input(target.widget(), text);
        failure = r ? std::string{} : r.error().message;
    } else {
        failure = "'action' must be one of click | scroll | text";
    }
    if (!failure.empty()) {
        out.error = failure;
        return out;
    }
    out.props = au::Inspector::get_prop(target.widget());
    out.snapshot = au::render_to_logical_snapshot(root, kWidth, kHeight);
    return out;
}

/// @brief 取属性快照中 `values` 段的某键（形状见 `get_widget_props`）。
[[nodiscard]] auto prop_value(const Simulation& s, const std::string& key) -> au::Json {
    if (!s.props.contains("values")) {
        return au::Json{};
    }
    return s.props["values"].value(key, au::Json{});
}

}  // namespace

AURORA_TEST_CASE(mcp_simulate_click_reports_target_state_change) {
    register_core_widgets();
    const au::Json tree =
        node_json("Column", au::Json::object(), au::Json::array({node_json("Checkbox")}));

    const Simulation s = simulate(tree, "click", "0");
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error.c_str());
    // 「已派发」不等于「状态变了」：必须读回属性确认事件真的落到了该控件（Checkbox 在
    // Release 时翻转 checked）。
    AURORA_TEST_CHECK(prop_value(s, "checked") == true);
    // 工具还会回传交互后的快照，供 AI 观察整棵树的后续状态。
    AURORA_TEST_CHECK(s.snapshot.contains("type"));
    AURORA_TEST_CHECK(s.snapshot.contains("children"));
}

AURORA_TEST_CASE(mcp_simulate_text_inserts_into_target) {
    register_core_widgets();
    const au::Json tree =
        node_json("Column", au::Json::object(), au::Json::array({node_json("TextInput")}));

    const Simulation s = simulate(tree, "text", "0", 0.0F, 0.0F, "hi");
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error.c_str());
    AURORA_TEST_CHECK(prop_value(s, "value") == "hi");
}

AURORA_TEST_CASE(mcp_simulate_scroll_reaches_layout_backed_target) {
    register_core_widgets();
    const au::Json content = node_json("Column", au::Json::object(),
                                       au::Json::array({node_json("Text", au::Json{{"content", "a"}}),
                                                        node_json("Text", au::Json{{"content", "b"}})}));
    const au::Json tree = node_json("Scroll", au::Json{{"step", 20.0}}, au::Json::array({content}));

    const Simulation s = simulate(tree, "scroll", "0", 0.0F, -24.0F);
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error.c_str());
    // 语义边界：滚动偏移不在这条通道的属性面上（Scroll 不序列化 offset；LazyList/GridView
    // 虽序列化 scroll_offset，但其子项由运行时 ItemBuilder 提供，静态 JSON 树给不出来），
    // 故此处断言的是「事件已派发到布局出的可命中目标」，偏移量本身须由 C++ 测试读回。
    AURORA_TEST_CHECK(s.snapshot.contains("type"));
}

AURORA_TEST_CASE(mcp_simulate_scroll_depends_on_the_layout_pass) {
    register_core_widgets();
    // 视口 600 高、内容远高于视口：滚动容器只有在布局后才算得出可滚动范围。
    au::Json rows = au::Json::array();
    for (int i = 0; i < 120; ++i) {
        rows.push_back(node_json("Text", au::Json{{"content", "row"}}));
    }
    const au::Json content = node_json("Column", au::Json::object(), std::move(rows));
    const au::Json tree = node_json("Scroll", au::Json{{"step", 20.0}}, au::Json::array({content}));

    // 未布局就派发：视口与内容尺寸都还是零，可滚动范围为零 → 滚轮事件空转，偏移不动。
    // 这正是工具在派发前必须先布局一次的理由。
    {
        auto widget = from_json(tree);
        AURORA_TEST_REQUIRE(widget.ok());
        au::Node root(std::move(widget.value()));
        au::Node target = au::Inspector::find_node(root, "");
        AURORA_TEST_REQUIRE(target);
        auto* scroll = dynamic_cast<au::Scroll*>(&target.widget());
        AURORA_TEST_REQUIRE(scroll != nullptr);
        (void)au::Inspector::simulate_scroll(target.widget(), 0.0F, -200.0F);
        AURORA_TEST_CHECK(scroll->offset_y() == 0.0F);
    }

    // 工具路径（派发前布局一次）：几何成立后同一事件真正改变滚动偏移。
    {
        const Simulation s = simulate(tree, "scroll", "", 0.0F, -200.0F);
        AURORA_TEST_CHECK_MSG(s.error.empty(), s.error.c_str());
        au::Node target = au::Inspector::find_node(s.root, "");
        AURORA_TEST_REQUIRE(target);
        auto* scroll = dynamic_cast<au::Scroll*>(&target.widget());
        AURORA_TEST_REQUIRE(scroll != nullptr);
        AURORA_TEST_CHECK(scroll->offset_y() > 0.0F);
    }
}

AURORA_TEST_CASE(mcp_simulate_reports_missing_path_hidden_target_and_unknown_action) {
    register_core_widgets();
    const au::Json tree =
        node_json("Column", au::Json::object(), au::Json::array({node_json("Checkbox")}));

    // 路径不存在：定位失败，不改动任何控件、也不产生快照。
    {
        const Simulation missing = simulate(tree, "click", "9");
        AURORA_TEST_CHECK(!missing.error.empty());
        AURORA_TEST_CHECK(missing.props.empty());
    }

    // 目标存在但整个子树不参与命中（show=false）：派发前即失败，不改变状态。
    {
        const au::Json hidden =
            node_json("Column", au::Json::object(),
                      au::Json::array({node_json("Checkbox", au::Json{{"show", false}})}));
        const Simulation s = simulate(hidden, "click", "0");
        AURORA_TEST_CHECK(!s.error.empty());
    }

    // 未知动作：工具侧在派发前即拒绝。
    {
        const Simulation s = simulate(tree, "hover", "0");
        AURORA_TEST_CHECK(!s.error.empty());
    }
}

// ---------- MCP 协议端到端测试（运行 aurora_mcp 可执行文件） ----------

AURORA_TEST_CASE(mcp_protocol_e2e_binary_smoke) {
    AURORA_TEST_REQUIRE_SUBPROCESS();
    const int ret = run_mcp_smoke();
    if (ret == -1) {
        AURORA_TEST_SKIP("aurora_mcp 未构建");
    }
    AURORA_TEST_CHECK(ret == 0 || ret == 1);  // stdin EOF 后正常退出
}

}  // namespace aurora::test_cases::itest_mcp
