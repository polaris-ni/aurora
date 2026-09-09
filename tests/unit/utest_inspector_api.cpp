/// 测试类型: unit
/// 目标单元: include/aurora/inspector/inspector_api.h
/// 测试说明: 覆盖 Inspector 统一门面——树查询四件套（text/rich/json/json_full）、widget_info
/// 与属性读写（get_prop_value 未命中返回 null、set_prop 容忍未知键）、apply_patch 路径补丁
/// 与非数组错误、query/find_node/get_state 定位、validate 错误→Diagnostic 映射、组件发现、
/// to_code、变化订阅生命周期、simulate_* 交互模拟（按当前实现派发事件并返回 ok）。

#include <string>

#include "aurora/inspector/inspector_api.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_inspector_api {

namespace {

/// @brief 构造确定性测试树：Column 根 + 单个 Text("hi") 子节点（无需布局即可检视）。
[[nodiscard]] auto make_tree() -> Node {
    auto col = std::make_shared<Column>();
    col->add(Node{std::make_shared<Text>("hi")});
    return Node{col};
}

}  // namespace

AURORA_TEST_CASE(tree_text_and_rich_dump_widget_tree) {
    Node root = make_tree();
    const std::string text = Inspector::tree_text(root);
    // 缩进树：每行一个 type_name，父子按层级缩进。
    AURORA_TEST_CHECK_TRUE(text.find("Column") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(text.find("Text") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(text.find('\n') != std::string::npos);

    // 富格式树：含文本内容与树形连接符。
    const std::string rich = Inspector::tree_rich(root);
    AURORA_TEST_CHECK_TRUE(rich.find("Column") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(rich.find("text: \"hi\"") != std::string::npos);
}

AURORA_TEST_CASE(tree_json_has_type_and_children_only) {
    const Json j = Inspector::tree_json(make_tree());
    AURORA_TEST_CHECK_EQ(j["type"], "Column");
    AURORA_TEST_CHECK_EQ(j["children"].size(), 1U);
    AURORA_TEST_CHECK_EQ(j["children"][0]["type"], "Text");
    AURORA_TEST_CHECK_FALSE(j.contains("props"));  // 结构化树仅 type + children
}

AURORA_TEST_CASE(tree_json_full_includes_props) {
    const Json j = Inspector::tree_json_full(make_tree());
    AURORA_TEST_CHECK_TRUE(j["props"].is_object());
    AURORA_TEST_CHECK_EQ(j["children"][0]["type"], "Text");
    // 完整快照携带序列化属性：Text 的 content 键为文本内容。
    AURORA_TEST_CHECK_EQ(j["children"][0]["props"]["content"], "hi");
}

AURORA_TEST_CASE(widget_info_and_prop_reads) {
    auto w = std::make_shared<Text>("hi");

    // widget_info：descriptor 元数据 + values 当前值合并。
    const Json info = Inspector::widget_info(*w);
    AURORA_TEST_CHECK_EQ(info["descriptor"]["name"], "Text");
    AURORA_TEST_CHECK_EQ(info["values"]["content"], "hi");

    // get_prop 与 widget_info 同构。
    const Json props = Inspector::get_prop(*w);
    AURORA_TEST_CHECK_EQ(props["descriptor"]["name"], "Text");
    AURORA_TEST_CHECK_EQ(props["values"]["content"], "hi");

    // 单键读取；未命中键返回 null Json（而非抛错）。
    AURORA_TEST_CHECK_EQ(Inspector::get_prop_value(*w, "content"), "hi");
    AURORA_TEST_CHECK_TRUE(Inspector::get_prop_value(*w, "no_such_key").is_null());
}

AURORA_TEST_CASE(set_prop_roundtrip_and_unknown_key_tolerance) {
    auto w = std::make_shared<Text>("hi");

    // 合法键往返：写入后读回一致。注意用圆括号构造——`Json{"changed"}` 是初始化列表
    // 语义（得到数组 ["changed"]），会触发 LocalizedString 类型校验失败降级。
    const Result<void> r = Inspector::set_prop(*w, "content", Json("changed"));
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(Inspector::get_prop_value(*w, "content"), "changed");

    // 未知键：deserialize 忽略，返回 ok 且原值不变。
    const Result<void> r2 = Inspector::set_prop(*w, "no_such_key", Json(1));
    AURORA_TEST_CHECK_TRUE(r2.ok());
    AURORA_TEST_CHECK_EQ(Inspector::get_prop_value(*w, "content"), "changed");
}

AURORA_TEST_CASE(apply_patch_sets_props_by_path) {
    Node root = make_tree();

    // 路径格式 "/<子节点索引路径>/<属性名>"：补丁作用于 0 号子节点的 content。
    const Json patch = Json::array({Json{{"path", "/0/content"}, {"value", "patched"}}});
    const Result<void> r = Inspector::apply_patch(root, patch);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(Inspector::get_prop_value(Inspector::find_node(root, "0").widget(), "content"), "patched");

    // 缺 value 的操作项被跳过（仍返回 ok）。
    const Json partial = Json::array({Json{{"path", "/0/content"}}});
    AURORA_TEST_CHECK_TRUE(Inspector::apply_patch(root, partial).ok());

    // 错误路径：补丁必须是 JSON 数组。
    const Result<void> bad = Inspector::apply_patch(root, Json::object());
    AURORA_TEST_REQUIRE_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, aurora::ErrorCode::GeneralNotSupported);
}

AURORA_TEST_CASE(query_and_find_node_by_path) {
    Node root = make_tree();

    // 按类型名精确匹配查询。
    AURORA_TEST_CHECK_EQ(Inspector::query("Text", root).size(), 1U);
    AURORA_TEST_CHECK_TRUE(Inspector::query("Button", root).empty());

    // 按索引路径定位："" 为根、"0" 为首子节点、越界返回空 Node。
    const Node by_root = Inspector::find_node(root, "");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(by_root));
    AURORA_TEST_CHECK_EQ(by_root.widget().type_name(), std::string_view{"Column"});
    const Node child = Inspector::find_node(root, "0");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(child));
    AURORA_TEST_CHECK_EQ(child.widget().type_name(), std::string_view{"Text"});
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(Inspector::find_node(root, "9")));
}

AURORA_TEST_CASE(get_state_walks_json_tree) {
    Node root = make_tree();
    // get_state 沿 dump_tree_json 产出的 JSON 树按 "/" 路径取片段。
    AURORA_TEST_CHECK_EQ(Inspector::get_state("type", root), "Column");
    AURORA_TEST_CHECK_EQ(Inspector::get_state("children/0/type", root), "Text");
    // 未命中路径返回空 Json。
    AURORA_TEST_CHECK_TRUE(Inspector::get_state("bogus/path", root).is_null());
}

AURORA_TEST_CASE(validate_maps_errors_to_diagnostics) {
    // 合法树：诊断列表为空。
    AURORA_TEST_CHECK_TRUE(Inspector::validate(make_tree()).empty());

    // 结构问题树：null 子节点映射为一条 Error 级 Diagnostic。
    auto col = std::make_shared<Column>();
    col->add(Node{});
    const std::vector<Diagnostic> diags = Inspector::validate(Node{col});
    AURORA_TEST_REQUIRE_EQ(diags.size(), 1U);
    AURORA_TEST_CHECK_EQ(diags[0].severity, aurora::ErrorSeverity::Error);
    AURORA_TEST_CHECK_TRUE(diags[0].message.find("null child") != std::string::npos);
}

AURORA_TEST_CASE(component_discovery_lists_registered_schemas) {
    // 组件发现：核心控件注册后 schema 列表非空，单组件 schema 携带类型名。
    const std::vector<Json> all = Inspector::components();
    AURORA_TEST_CHECK_FALSE(all.empty());

    const Json schema = Inspector::component_schema("Text");
    AURORA_TEST_CHECK_TRUE(schema.is_object());
    AURORA_TEST_CHECK_EQ(schema["type"], "Text");
}

AURORA_TEST_CASE(to_code_generates_source_from_tree) {
    // 代码生成：树转源码输出非空可读文本。
    const std::string code = Inspector::to_code(make_tree());
    AURORA_TEST_CHECK_GT(code.size(), 0U);
    AURORA_TEST_CHECK_TRUE(code.find("Column") != std::string::npos);
}

AURORA_TEST_CASE(subscribe_notify_unsubscribe_cycle) {
    int hits1 = 0;
    int hits2 = 0;
    const std::size_t id1 = Inspector::subscribe_changes([&hits1](const Json&) -> void { ++hits1; });
    const std::size_t id2 = Inspector::subscribe_changes([&hits2](const Json&) -> void { ++hits2; });
    AURORA_TEST_CHECK_NE(id1, id2);  // 订阅 id 唯一递增

    Inspector::notify_changes(Json::object());
    AURORA_TEST_CHECK_EQ(hits1, 1);
    AURORA_TEST_CHECK_EQ(hits2, 1);

    // 取消 id1 后仅 id2 收到通知。
    Inspector::unsubscribe(id1);
    Inspector::notify_changes(Json::object());
    AURORA_TEST_CHECK_EQ(hits1, 1);
    AURORA_TEST_CHECK_EQ(hits2, 2);

    Inspector::unsubscribe(id2);  // 清理订阅，防悬垂
    Inspector::unsubscribe(id1);  // 重复取消安全（erase 不存在键为 no-op）
}

AURORA_TEST_CASE(simulate_helpers_dispatch_and_return_ok) {
    // 交互模拟：当前实现经 EventDispatcher 在控件自身中心派发事件并返回 ok
    // （注意：头文件注释「当前返回 GeneralNotSupported」与实现不符，以运行时行为为准）。
    auto w = std::make_shared<Text>("hi");
    AURORA_TEST_CHECK_NO_THROW((void)Inspector::simulate_click(*w));
    AURORA_TEST_CHECK_NO_THROW((void)Inspector::simulate_scroll(*w, 0.0F, -10.0F));
    AURORA_TEST_CHECK_NO_THROW((void)Inspector::simulate_text_input(*w, "abc"));

    const Result<void> click = Inspector::simulate_click(*w);
    AURORA_TEST_CHECK_TRUE(click.ok());
}

}  // namespace aurora::test_cases::utest_inspector_api
