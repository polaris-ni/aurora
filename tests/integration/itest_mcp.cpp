/// 测试类型: integration
/// 目标单元: include/aurora/widget/serialization.h
/// 测试说明: aurora-mcp 工具链测试——库 API（list_all_components/describe_component/
///           search_components/from_json+validate/render_to_logical_snapshot/to_code/
///           list_all_schemas/simulate_*）直测 MCP 工具消费的逻辑；协议端到端按构建产物
///           探测，未构建则 SKIP。另有一条真 stdio 往返：拉起 aurora_mcp、喂一批 JSON-RPC
///           帧，核对工具清单 24 枚与 network 两枚工具的 Inspector 回环（过滤、多跳展开与
///           未知 id 的 404 映射），夹具经进程级活动存储直接写入。
/// 覆盖说明: aurora_mcp 是独立可执行（tools/servers/），协议用例要求该产物已构建且
///           Inspector server 已编入；两者缺一即 SKIP。

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/commands.h"
#include "aurora/core/platform.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "command_listing.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "framework/json_literals.h"
#include "paths.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER
#include "aurora/inspector/inspector_server.h"
#include "aurora/net/detail/observability_trace.h"
#include "aurora/net/http_observability.h"
#endif

namespace aurora::test_cases::itest_mcp {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

using au::serialization::CodeStyle;
using au::serialization::from_json;
using au::serialization::register_core_widgets;
using au::serialization::to_code;

namespace {

/// @brief 定位自构建的 aurora_mcp 可执行文件；未构建返回空串。
[[nodiscard]] auto locate_mcp_binary() -> std::string {
    for (const char *name : {"aurora_mcp.exe", "aurora_mcp"}) {
        const std::string path = au::testing::paths::under_repo(std::string{"build/"} + name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            return path;
        }
    }
    return {};
}

// MCP 端到端：stdin 给 EOF 即退出（固定冒烟命令拉起自构建二进制，输出重定向 null 设备）。
auto run_mcp_smoke() -> int {
    const std::string exe = locate_mcp_binary();
    if (exe.empty()) {
        return -1;  // 未构建
    }
#ifdef AURORA_PLATFORM_WINDOWS
    const char *null_dev = "<nul >nul 2>&1";
#else
    const char *null_dev = "</dev/null >/dev/null 2>&1";
#endif
    const std::string cmd = "\"" + exe + "\" " + null_dev;
    // 测试用意拉起自构建二进制，命令固定且不含外部输入。
    // NOLINTNEXTLINE(bugprone-command-processor)
    return std::system(cmd.c_str());
}

#ifdef AURORA_BUILD_INSPECTOR_SERVER

/// @brief 每例前后把进程级活动存储与配置清回缺省（Release 下两枚函数是 no-op）。
class ScopedActivityReset {
  public:
    ScopedActivityReset() { reset(); }
    ~ScopedActivityReset() { reset(); }
    ScopedActivityReset(const ScopedActivityReset &) = delete;
    auto operator=(const ScopedActivityReset &) -> ScopedActivityReset & = delete;
    ScopedActivityReset(ScopedActivityReset &&) = delete;
    auto operator=(ScopedActivityReset &&) -> ScopedActivityReset & = delete;

  private:
    static auto reset() -> void {
        aurora::net::http::clear_network_activity();
        aurora::net::http::set_activity_options(aurora::net::http::ActivityOptions{});
    }
};

/// @brief 向进程级活动存储写入一笔已收摊的交换；MCP 子进程经 Inspector 端点读回同一份数据。
/// @param method 请求方法。
/// @param request_id 链路标识。
/// @param url 目标 URL。
/// @param hop 重定向跳序号。
/// @param pass 物理尝试序号。
/// @param status 应答状态码。
auto seed_network_activity(std::string_view method, std::string_view request_id, std::string_view url,
                           std::uint16_t hop, std::uint16_t pass, std::uint16_t status) -> void {
    auto parsed = aurora::Url::parse(url);
    AURORA_TEST_REQUIRE_TRUE(parsed.ok());
    aurora::net::http::ExchangeTrace trace{method, request_id, parsed.value(), hop, pass, false, false};
    trace.mark(aurora::net::http::HttpStage::ConnectionAcquired);
    trace.mark(aurora::net::http::HttpStage::RequestSent);
    trace.set_status(status);
    trace.mark(aurora::net::http::HttpStage::FirstByte);
    trace.mark(aurora::net::http::HttpStage::BodyDone);
    trace.finish();
}

/// @brief 一批请求写临时文件、stdin 重定向、stdout 走管道，拉起 MCP 并读回全部输出。
///
/// 子进程读到 EOF 即收摊，故一次调用可完整覆盖 tools/list 与多条 tools/call；不需要交互式
/// 双向管道。命令只拼自构建产物路径与本用例临时文件路径，不含外部输入。
/// @param exe 可执行文件路径。
/// @param requests 全部 JSON-RPC 帧（按 Content-Length 分帧）。
/// @return 子进程的标准输出原文。
auto run_mcp_batch(const std::string &exe, const std::string &requests) -> std::string {
    const std::filesystem::path input =
        std::filesystem::path{aurora::testing::isolation::temp_dir()} / "mcp-batch-requests.txt";
    {
        std::ofstream out(input, std::ios::binary | std::ios::trunc);
        AURORA_TEST_REQUIRE(out.good());
        out << requests;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    // `call` 前缀不是可省的装饰：MinGW 的 `_popen` 把整条命令再交给 `cmd /c`，命令以引号开头
    // 时会踩引号剥离规则，重定向被吞掉、子进程输出为空；让首 token 变成 `call` 即可稳定解析。
    const std::string command = "call \"" + exe + "\" < \"" + input.string() + "\" 2>nul";
    // 测试用意拉起自构建二进制，命令固定且不含外部输入。
    // NOLINTNEXTLINE(bugprone-command-processor)
    std::unique_ptr<FILE, decltype(&_pclose)> pipe{_popen(command.c_str(), "rb"), &_pclose};
#else
    const std::string command = "\"" + exe + "\" < \"" + input.string() + "\" 2>/dev/null";
    // 测试用意拉起自构建二进制，命令固定且不含外部输入。
    // NOLINTNEXTLINE(bugprone-command-processor)
    std::unique_ptr<FILE, decltype(&pclose)> pipe{popen(command.c_str(), "r"), &pclose};
#endif
    AURORA_TEST_REQUIRE(pipe != nullptr);
    std::string output;
    char buffer[4096];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), pipe.get());
        if (got == 0) {
            break;
        }
        output.append(buffer, got);
    }
    return output;
}

/// @brief 拼一枚 JSON-RPC 帧（Content-Length 头 + 体）。
/// @param id 消息 id。
/// @param method JSON-RPC 方法。
/// @param params 参数；Null 表示不写 params。
/// @return 可直接写进子进程 stdin 的帧。
auto rpc_frame(int id, std::string_view method, const au::Json &params) -> std::string {
    au::Json request = au::Json::object();
    request.set("jsonrpc", "2.0");
    request.set("id", id);
    request.set("method", std::string{method});
    if (!params.is_null()) {
        request.set("params", params);
    }
    const auto dumped = au::json::dump(request);
    const std::string body = dumped.ok() ? dumped.value() : std::string{};
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// @brief 拼一枚 tools/call 帧。
/// @param id 消息 id。
/// @param tool 工具名。
/// @param arguments 工具参数对象。
/// @return 帧文本。
auto tool_call_frame(int id, std::string_view tool, const au::Json &arguments) -> std::string {
    au::Json params = au::Json::object();
    params.set("name", std::string{tool});
    params.set("arguments", arguments);
    return rpc_frame(id, "tools/call", params);
}

/// @brief 从 MCP 的 stdout 批量输出里切出全部 JSON-RPC 消息。
/// @param raw 原始输出。
/// @return 按出现次序排列的已解析消息；分帧不完整处停止。
auto parse_mcp_messages(std::string_view raw) -> std::vector<au::Json> {
    std::vector<au::Json> messages;
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const auto header_end = raw.find("\r\n\r\n", offset);
        if (header_end == std::string_view::npos) {
            break;
        }
        const std::string_view header = raw.substr(offset, header_end - offset);
        const auto marker = header.find("Content-Length:");
        if (marker == std::string_view::npos) {
            break;
        }
        std::size_t digits_begin = marker + 15;
        while (digits_begin < header.size() && header[digits_begin] == ' ') {
            ++digits_begin;
        }
        std::size_t length = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 要 [first, last) 两端
        const auto parsed_length = std::from_chars(header.data() + digits_begin, header.data() + header.size(), length);
        if (parsed_length.ec != std::errc{}) {
            break;
        }
        const auto body_begin = header_end + 4;
        if (body_begin + length > raw.size()) {
            break;
        }
        auto parsed = au::json::parse(raw.substr(body_begin, length));
        if (parsed) {
            messages.push_back(std::move(parsed).value());
        }
        offset = body_begin + length;
    }
    return messages;
}

/// @brief 从 tools/call 响应里取回工具载荷：Inspector 的 JSON 原文在 content[0].text。
/// @param message 一条 JSON-RPC 响应。
/// @return 解析后的工具载荷；响应带 isError 或文本不可解析即本用例致命失败。
auto tool_payload(const au::Json &message) -> au::Json {
    const au::Json &result = *require_child(message, "result");
    AURORA_TEST_REQUIRE(result.find("isError") == nullptr);
    const au::Json &content = *require_child(result, "content");
    const au::Json &text_item = *require_child_at(content, 0);
    const auto text = require_field<std::string>(text_item, "text");
    auto parsed = au::json::parse(text);
    AURORA_TEST_REQUIRE_TRUE(parsed.ok());
    return parsed.value();
}

#endif  // AURORA_BUILD_INSPECTOR_SERVER

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

AURORA_TEST_CASE(mcp_describe_component_returns_full_schema) {
    const au::Json schema = au::describe_component("Button");
    AURORA_TEST_CHECK(!schema.empty());
    AURORA_TEST_CHECK(schema.contains("type"));
    AURORA_TEST_CHECK(require_field<std::string>(schema, "type") == "Button");
    AURORA_TEST_CHECK(schema.contains("props"));
    AURORA_TEST_CHECK(require_child(schema, "props")->is_array());
    AURORA_TEST_CHECK(!require_child(schema, "props")->empty());
    AURORA_TEST_CHECK(schema.contains("events"));
    AURORA_TEST_CHECK(schema.contains("children_policy"));
    AURORA_TEST_CHECK(require_field<std::string>(schema, "children_policy") == "none");

    // 未知组件：返回空 Json 对象或不含有效 props 条目。
    const au::Json unknown = au::describe_component("NonExistentWidget");
    const auto *unknown_desc = unknown.find("props");
    AURORA_TEST_CHECK(unknown_desc == nullptr || unknown_desc->empty());
}

AURORA_TEST_CASE(mcp_search_components_finds_by_substring) {
    const auto results = au::search_components("but");
    AURORA_TEST_CHECK(!results.empty());
    bool found = false;
    for (const auto &r : results) {
        if (r.as_or<std::string>("type", "") == "Button") {
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
    valid_tree.set("type", "Column");
    valid_tree.set("props", au::Json::object());
    valid_tree.set("children", au::Json::array());

    auto widget = from_json(valid_tree);
    AURORA_TEST_CHECK(widget.ok());
    if (widget.ok()) {
        au::Node root(std::move(widget.value()));
        const auto ok = au::validate(root);
        AURORA_TEST_CHECK(ok.ok());
    }

    // 非法树（未知类型）：from_json 对未知类型可能返回错误或降级。
    au::Json invalid_tree = au::Json::object();
    invalid_tree.set("type", "UnknownWidget");
    invalid_tree.set("props", au::Json::object());
    invalid_tree.set("children", au::Json::array());

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
    tree.set("type", "Text");
    tree.set("props", testing::json_obj({{"content", "Hello"}}));
    tree.set("children", au::Json::array());

    auto widget = from_json(tree);
    AURORA_TEST_CHECK(widget.ok());
    if (!widget.ok()) {
        return;
    }
    au::Node root(std::move(widget.value()));
    const au::Json snapshot = au::render_to_logical_snapshot(root, 400, 300);
    AURORA_TEST_CHECK(snapshot.contains("type"));
    AURORA_TEST_CHECK(require_field<std::string>(snapshot, "type") == "Text");
    AURORA_TEST_CHECK(snapshot.contains("box"));
    AURORA_TEST_CHECK(require_child(snapshot, "box")->contains("w"));
    AURORA_TEST_CHECK(require_child(snapshot, "box")->contains("h"));
}

AURORA_TEST_CASE(mcp_to_code_generates_fluent_and_designated_init) {
    au::Json tree = au::Json::object();
    tree.set("type", "Button");
    tree.set("props", testing::json_obj({{"label", "OK"}}));
    tree.set("children", au::Json::array());

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

    // 每个 schema 含 type 和 props 表。
    for (const auto &s : schemas) {
        AURORA_TEST_CHECK(s.contains("type"));
        AURORA_TEST_CHECK(s.contains("props"));
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

[[nodiscard]] auto node_json(const std::string &type, au::Json props = au::Json::object(),
                             au::Json children = au::Json::array()) -> au::Json {
    au::Json n = au::Json::object();
    n.set("type", type);
    n.set("props", std::move(props));
    n.set("children", std::move(children));
    return n;
}

/// @brief 复刻 `simulate_interaction` 的消费路径。
[[nodiscard]] auto simulate(const au::Json &tree, const std::string &action, const std::string &path, float dx = 0.0F,
                            float dy = 0.0F, const std::string &text = {}) -> Simulation {
    constexpr int width = 800;
    constexpr int height = 600;
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
    (void)au::render_to_logical_snapshot(root, width, height);
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
    out.snapshot = au::render_to_logical_snapshot(root, width, height);
    return out;
}

/// @brief 取属性快照中 `values` 段的某键（形状见 `get_widget_props`）。
[[nodiscard]] auto prop_value(const Simulation &s, const std::string &key) -> au::Json {
    const auto *values = s.props.find("values");
    if (values == nullptr) {
        return au::Json{};
    }
    const auto *v = values->find(key);
    return v != nullptr ? *v : au::Json{};
}

}  // namespace

AURORA_TEST_CASE(mcp_simulate_click_reports_target_state_change) {
    register_core_widgets();
    const au::Json tree = node_json("Column", au::Json::object(), testing::json_arr({node_json("Checkbox")}));

    const Simulation s = simulate(tree, "click", "0");
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error);
    // 「已派发」不等于「状态变了」：必须读回属性确认事件真的落到了该控件（Checkbox 在
    // Release 时翻转 checked）。
    AURORA_TEST_CHECK(prop_value(s, "checked") == au::Json{true});
    // 工具还会回传交互后的快照，供 AI 观察整棵树的后续状态。
    AURORA_TEST_CHECK(s.snapshot.contains("type"));
    AURORA_TEST_CHECK(s.snapshot.contains("children"));
}

AURORA_TEST_CASE(mcp_simulate_text_inserts_into_target) {
    register_core_widgets();
    const au::Json tree = node_json("Column", au::Json::object(), testing::json_arr({node_json("TextInput")}));

    const Simulation s = simulate(tree, "text", "0", 0.0F, 0.0F, "hi");
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error);
    AURORA_TEST_CHECK(prop_value(s, "value") == "hi");
}

AURORA_TEST_CASE(mcp_simulate_scroll_reaches_layout_backed_target) {
    register_core_widgets();
    const au::Json content = node_json("Column", au::Json::object(),
                                       testing::json_arr({node_json("Text", testing::json_obj({{"content", "a"}})),
                                                          node_json("Text", testing::json_obj({{"content", "b"}}))}));
    const au::Json tree = node_json("Scroll", testing::json_obj({{"step", 20.0}}), testing::json_arr({content}));

    const Simulation s = simulate(tree, "scroll", "0", 0.0F, -24.0F);
    AURORA_TEST_CHECK_MSG(s.error.empty(), s.error);
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
        rows.push_back(node_json("Text", testing::json_obj({{"content", "row"}})));
    }
    const au::Json content = node_json("Column", au::Json::object(), std::move(rows));
    const au::Json tree = node_json("Scroll", testing::json_obj({{"step", 20.0}}), testing::json_arr({content}));

    // 未布局就派发：视口与内容尺寸都还是零，可滚动范围为零 → 滚轮事件空转，偏移不动。
    // 这正是工具在派发前必须先布局一次的理由。
    {
        auto widget = from_json(tree);
        AURORA_TEST_REQUIRE(widget.ok());
        au::Node root(std::move(widget.value()));
        au::Node target = au::Inspector::find_node(root, "");
        AURORA_TEST_REQUIRE(target);
        auto *scroll = dynamic_cast<au::Scroll *>(&target.widget());
        AURORA_TEST_REQUIRE(scroll != nullptr);
        (void)au::Inspector::simulate_scroll(target.widget(), 0.0F, -200.0F);
        AURORA_TEST_CHECK(scroll->offset_y() == 0.0F);
    }

    // 工具路径（派发前布局一次）：几何成立后同一事件真正改变滚动偏移。
    {
        const Simulation s = simulate(tree, "scroll", "", 0.0F, -200.0F);
        AURORA_TEST_CHECK_MSG(s.error.empty(), s.error);
        au::Node target = au::Inspector::find_node(s.root, "");
        AURORA_TEST_REQUIRE(target);
        auto *scroll = dynamic_cast<au::Scroll *>(&target.widget());
        AURORA_TEST_REQUIRE(scroll != nullptr);
        AURORA_TEST_CHECK(scroll->offset_y() > 0.0F);
    }
}

AURORA_TEST_CASE(mcp_simulate_reports_missing_path_hidden_target_and_unknown_action) {
    register_core_widgets();
    const au::Json tree = node_json("Column", au::Json::object(), testing::json_arr({node_json("Checkbox")}));

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
                      testing::json_arr({node_json("Checkbox", testing::json_obj({{"show", au::Json{false}}}))}));
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
        AURORA_TEST_SKIP("aurora_mcp not built");
    }
    AURORA_TEST_CHECK(ret == 0 || ret == 1);  // stdin EOF 后正常退出
}

AURORA_TEST_CASE(mcp_network_tools_roundtrip_through_inspector) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP("requires the inspector server, worker threads and BSD sockets");
#else
    AURORA_TEST_REQUIRE_SUBPROCESS();
    const std::string exe = locate_mcp_binary();
    if (exe.empty()) {
        AURORA_TEST_SKIP("aurora_mcp not built");
    }
    ScopedActivityReset reset;

    // 夹具写进本进程的活动存储；MCP 子进程经本地 Inspector 端点读回同一份数据，两枚网络
    // 工具走的正是对用户开放的通道。同一 request_id 的两跳（302 → 200）覆盖多跳展开，
    // 另一笔 503 覆盖 host/status 过滤。
    seed_network_activity("GET", "rid-hop", "https://api.example.test/start", 0, 0, 302);
    seed_network_activity("GET", "rid-filter", "https://api.example.test/two", 0, 0, 503);
    seed_network_activity("GET", "rid-hop", "https://api.example.test/final", 1, 0, 200);

    const auto column = std::make_shared<au::Column>();
    column->add(au::Node{std::make_shared<au::Text>("network")});
    aurora::InspectorServer server{[column]() -> au::Node { return au::Node{column}; }};
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::string session = "127.0.0.1:" + std::to_string(server.port());

    au::Json default_args = au::Json::object();
    default_args.set("session", session);
    au::Json filtered_args = au::Json::object();
    filtered_args.set("session", session);
    filtered_args.set("host", "API.EXAMPLE.TEST");  // 大小写不敏感由端点负责
    filtered_args.set("status", 503);
    au::Json describe_args = au::Json::object();
    describe_args.set("session", session);
    describe_args.set("request_id", "rid-hop");
    au::Json unknown_args = au::Json::object();
    unknown_args.set("session", session);
    unknown_args.set("request_id", "probe-secret-identifier");

    const std::string requests = rpc_frame(1, "tools/list", au::Json{}) +
                                 tool_call_frame(2, "list_network_activity", default_args) +
                                 tool_call_frame(3, "list_network_activity", filtered_args) +
                                 tool_call_frame(4, "describe_network_request", describe_args) +
                                 tool_call_frame(5, "describe_network_request", unknown_args);
    const std::vector<au::Json> messages = parse_mcp_messages(run_mcp_batch(exe, requests));
    AURORA_TEST_REQUIRE_EQ(messages.size(), std::size_t{5});

    // tools/list：两枚网络工具已注册，工具总数 22 → 24。
    const au::Json &tools = *require_child(*require_child(messages[0], "result"), "tools");
    AURORA_TEST_CHECK_EQ(tools.size(), std::size_t{24});
    bool has_list = false;
    bool has_describe = false;
    for (const au::Json &tool : tools) {
        const auto name = tool.as_or<std::string>("name", "");
        has_list = has_list || name == "list_network_activity";
        has_describe = has_describe || name == "describe_network_request";
    }
    AURORA_TEST_CHECK_TRUE(has_list);
    AURORA_TEST_CHECK_TRUE(has_describe);

#ifdef AURORA_ENABLE_DEBUG
    // 缺省 limit：全部三笔，最新在前（rid-hop 的第二跳最后写入）。
    const au::Json listed = tool_payload(messages[1]);
    AURORA_TEST_CHECK_TRUE(listed.as_or<bool>("available", false));
    AURORA_TEST_CHECK_EQ(listed.as_or<std::int64_t>("count", -1), std::int64_t{3});
    const au::Json &entries = *require_child(listed, "entries");
    AURORA_TEST_REQUIRE_EQ(entries.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(entries, 0), "request_id"),
                         std::string{"rid-hop"});
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(*require_child_at(entries, 0), "hop"), std::int64_t{1});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(entries, 1), "request_id"),
                         std::string{"rid-filter"});

    // 按 host + status 过滤：只剩 503 那笔。
    const au::Json filtered = tool_payload(messages[2]);
    AURORA_TEST_CHECK_TRUE(filtered.as_or<bool>("available", false));
    AURORA_TEST_CHECK_EQ(filtered.as_or<std::int64_t>("count", -1), std::int64_t{1});
    const au::Json &hit = *require_child_at(*require_child(filtered, "entries"), 0);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(hit, "request_id"), std::string{"rid-filter"});
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(hit, "status"), std::int64_t{503});

    // 指定 request_id：条目取最新一跳，时间线按 hop 升序展开两跳。
    const au::Json described = tool_payload(messages[3]);
    AURORA_TEST_CHECK_TRUE(described.as_or<bool>("available", false));
    AURORA_TEST_CHECK_EQ(require_field<std::string>(described, "request_id"), std::string{"rid-hop"});
    const au::Json &latest = *require_child(described, "entry");
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(latest, "hop"), std::int64_t{1});
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(latest, "status"), std::int64_t{200});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(latest, "path"), std::string{"/final"});
    const au::Json &timelines = *require_child(described, "timelines");
    AURORA_TEST_REQUIRE_EQ(timelines.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(*require_child_at(timelines, 0), "hop"), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(*require_child_at(timelines, 1), "hop"), std::int64_t{1});
    const au::Json &marks = *require_child(*require_child_at(timelines, 0), "marks");
    AURORA_TEST_REQUIRE_TRUE(!marks.empty());
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(marks, 0), "stage"),
                         std::string{"connection_acquired"});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(marks, marks.size() - 1), "stage"),
                         std::string{"body_done"});

    // 未知 id：工具报错只给状态码，不回传调用方的查询串。
    const au::Json &unknown_result = *require_child(messages[4], "result");
    AURORA_TEST_CHECK_TRUE(unknown_result.as_or<bool>("isError", false));
    const auto unknown_text =
        require_field<std::string>(*require_child_at(*require_child(unknown_result, "content"), 0), "text");
    AURORA_TEST_CHECK_TRUE(unknown_text.find("HTTP 404") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(unknown_text.find("probe-secret-identifier") == std::string::npos);
#else
    // Release：两枚工具仍可调用，端点给确定形态的 unavailable，而不是 404 或空表。
    for (const au::Json &message : {messages[1], messages[2], messages[3], messages[4]}) {
        const au::Json payload = tool_payload(message);
        AURORA_TEST_CHECK_FALSE(payload.as_or<bool>("available", true));
        AURORA_TEST_CHECK_EQ(payload.as_or<std::string>("reason", std::string{}),
                             std::string{"AURORA_ENABLE_DEBUG not enabled"});
    }
#endif
    server.stop();
#endif
}

// ---------- 命令枚举 / 解析工具（无状态：描述符随请求传入） ----------

AURORA_TEST_CASE(mcp_list_commands_ranks_and_filters_like_the_palette) {
    au::CommandRegistry commands;
    au::Command open;
    open.id = "file.open";
    open.title = "Open File";
    open.action = []() -> void {};
    commands.add(std::move(open));
    au::Command copy;
    copy.id = "file.copy";
    copy.title = "Copy";
    copy.action = []() -> void {};
    commands.add(std::move(copy));
    au::Command hidden;
    hidden.id = "file.close";
    hidden.title = "Close File";
    hidden.enabled = []() -> bool { return false; };
    commands.add(std::move(hidden));

    const au::Json envelope = commands.to_json();
    const au::Json *items = au::tools::command_descriptors(envelope);
    AURORA_TEST_REQUIRE_TRUE(items != nullptr);
    AURORA_TEST_CHECK_EQ(items->size(), std::size_t{3});

    // 空查询：仅启用者，标题升序（"Copy" < "Open File"）。
    const au::tools::CommandListing all = au::tools::list_commands(*items, "", false);
    AURORA_TEST_CHECK_EQ(all.indices.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(all.considered, std::size_t{3});  // 形态合法的描述符总数（含未启用）
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*items, all.indices[0]), "id"),
                         std::string{"file.copy"});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*items, all.indices[1]), "id"),
                         std::string{"file.open"});

    // 模糊查询 "op"：词首命中者排前（与命令面板同一打分与排序）。
    const au::tools::CommandListing hits = au::tools::list_commands(*items, "op", false);
    AURORA_TEST_CHECK_EQ(hits.indices.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*items, hits.indices[0]), "id"),
                         std::string{"file.open"});
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*items, hits.indices[1]), "id"),
                         std::string{"file.copy"});

    // 未启用者默认被过滤，显式放开后可见。
    AURORA_TEST_CHECK_EQ(au::tools::list_commands(*items, "clo", false).indices.size(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(au::tools::list_commands(*items, "clo", true).indices.size(), std::size_t{1});

    // 非子序列无命中。
    AURORA_TEST_CHECK_EQ(au::tools::list_commands(*items, "zzz", false).indices.size(), std::size_t{0});
}

AURORA_TEST_CASE(mcp_invoke_command_reports_status_without_claiming_execution) {
    au::CommandRegistry commands;
    au::Command runnable;
    runnable.id = "file.open";
    runnable.title = "Open File";
    runnable.action = []() -> void {};
    commands.add(std::move(runnable));
    au::Command inert;
    inert.id = "file.placeholder";
    inert.title = "Placeholder";
    commands.add(std::move(inert));
    au::Command gated;
    gated.id = "file.close";
    gated.title = "Close File";
    gated.enabled = []() -> bool { return false; };
    gated.action = []() -> void {};
    commands.add(std::move(gated));

    const au::Json envelope = commands.to_json();
    const au::Json *items = au::tools::command_descriptors(envelope);
    AURORA_TEST_REQUIRE_TRUE(items != nullptr);

    au::tools::CommandStatus status = au::tools::CommandStatus::NotFound;

    const au::Json *found = au::tools::resolve_command(*items, "file.open", status);
    AURORA_TEST_REQUIRE_TRUE(found != nullptr);
    AURORA_TEST_CHECK_TRUE(status == au::tools::CommandStatus::Invocable);
    AURORA_TEST_CHECK_TRUE(au::tools::command_bool_field(*found, "invocable", false));

    found = au::tools::resolve_command(*items, "file.placeholder", status);
    AURORA_TEST_REQUIRE_TRUE(found != nullptr);
    AURORA_TEST_CHECK_TRUE(status == au::tools::CommandStatus::NotInvocable);
    AURORA_TEST_CHECK_FALSE(au::tools::command_bool_field(*found, "invocable", false));

    found = au::tools::resolve_command(*items, "file.close", status);
    AURORA_TEST_REQUIRE_TRUE(found != nullptr);
    AURORA_TEST_CHECK_TRUE(status == au::tools::CommandStatus::Disabled);
    AURORA_TEST_CHECK_FALSE(au::tools::command_bool_field(*found, "enabled", true));

    found = au::tools::resolve_command(*items, "no.such.command", status);
    AURORA_TEST_CHECK_NULL(found);
    AURORA_TEST_CHECK_TRUE(status == au::tools::CommandStatus::NotFound);

    // 状态标识是工具面的稳定契约。
    AURORA_TEST_CHECK_EQ(std::string{au::tools::command_status_name(au::tools::CommandStatus::Invocable)},
                         std::string{"invocable"});
    AURORA_TEST_CHECK_EQ(std::string{au::tools::command_status_name(au::tools::CommandStatus::Disabled)},
                         std::string{"disabled"});
    AURORA_TEST_CHECK_EQ(std::string{au::tools::command_status_name(au::tools::CommandStatus::NotInvocable)},
                         std::string{"not-invocable"});
    AURORA_TEST_CHECK_EQ(std::string{au::tools::command_status_name(au::tools::CommandStatus::NotFound)},
                         std::string{"not-found"});
}

AURORA_TEST_CASE(mcp_command_descriptors_accepts_envelope_and_bare_array) {
    au::CommandRegistry commands;
    au::Command one;
    one.id = "a.one";
    one.title = "One";
    one.action = []() -> void {};
    commands.add(std::move(one));

    const au::Json envelope = commands.to_json();
    const au::Json *from_envelope = au::tools::command_descriptors(envelope);
    AURORA_TEST_REQUIRE_TRUE(from_envelope != nullptr);
    AURORA_TEST_CHECK_EQ(from_envelope->size(), std::size_t{1});

    const au::Json &bare = *require_child(envelope, "commands");
    AURORA_TEST_CHECK_TRUE(au::tools::command_descriptors(bare) != nullptr);

    // 形态不符：无 commands 键的对象 / 标量 → 不识别。
    AURORA_TEST_CHECK_NULL(au::tools::command_descriptors(au::Json::object()));
    AURORA_TEST_CHECK_NULL(au::tools::command_descriptors(au::Json(42)));
}

}  // namespace aurora::test_cases::itest_mcp
