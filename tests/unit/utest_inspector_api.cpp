/// 测试类型: unit
/// 目标单元: include/aurora/inspector/inspector_api.h
/// 测试说明: 覆盖 Inspector 统一门面——树查询四件套（text/rich/json/json_full）、widget_info
/// 与属性读写（get_prop_value 未命中返回 null、set_prop 容忍未知键）、apply_patch 路径补丁
/// 与非数组错误、query/find_node/get_state 定位、validate 错误→Diagnostic 映射、组件发现、
/// to_code、变化订阅生命周期、simulate_* 交互模拟（点击计数 / 获焦、文本落字、滚动偏移等
/// 状态变化与不可命中时的错误返回）。

#include <memory>
#include <string>

#include "aurora/inspector/inspector_api.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_inspector_api {

namespace {

/// @brief 构造确定性测试树：Column 根 + 单个 Text("hi") 子节点（无需布局即可检视）。
[[nodiscard]] auto make_tree() -> Node {
    auto col = std::make_shared<Column>();
    col->add(Node{std::make_shared<Text>("hi")});
    return Node{col};
}

/// @brief 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto box(float w, float h) -> Node { return Node{std::make_shared<FixedBox>(w, h)}; }

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
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

AURORA_TEST_CASE(simulate_click_fires_button_handler_once) {
    // 目标式派发：以控件自身为根、指针取控件中心，一次完整 Press+Release 只触发一次点击。
    Button btn("OK");
    int clicks = 0;
    btn.set_on_click([&clicks]() -> void { ++clicks; });
    LayoutEngine::layout(btn, bounded(120.0F, 40.0F));

    const Result<void> r = Inspector::simulate_click(btn);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(clicks, 1);
}

AURORA_TEST_CASE(simulate_click_moves_focus_to_target) {
    // 合成指针事件携带焦点管理器：否则 request_focus() 静默 no-op，点击不会转移焦点。
    TextInput ti;
    LayoutEngine::layout(ti, bounded(200.0F, 40.0F));
    AURORA_TEST_CHECK_FALSE(ti.is_focused());

    const Result<void> r = Inspector::simulate_click(ti);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_TRUE(ti.is_focused());
}

AURORA_TEST_CASE(simulate_text_input_writes_into_target) {
    // 文本落到被指定为目标的控件（内部先置焦，on_text_input 以 is_focused 为前提）。
    TextInput ti;
    LayoutEngine::layout(ti, bounded(200.0F, 40.0F));

    const Result<void> r = Inspector::simulate_text_input(ti, "abc");
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_EQ(ti.value(), std::string{"abc"});
}

AURORA_TEST_CASE(simulate_scroll_moves_offset_and_clamps) {
    // 以滚动容器为目标派发滚轮事件：delta_y 为负使偏移增大 |delta|×step，且钳制在可滚范围内。
    Scroll sc{ScrollProps{.child = box(300.0F, 800.0F), .step = 10.0F}};
    LayoutEngine::layout(sc, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(sc.offset_y(), 0.0F, 1e-4F);

    const Result<void> r = Inspector::simulate_scroll(sc, 0.0F, -30.0F);
    AURORA_TEST_REQUIRE_TRUE(r.ok());
    AURORA_TEST_CHECK_NEAR(sc.offset_y(), 300.0F, 1e-4F);

    // 反向滚动回顶：可滚范围 800-200=600，越界被钳制。
    AURORA_TEST_REQUIRE_TRUE(Inspector::simulate_scroll(sc, 0.0F, 1000.0F).ok());
    AURORA_TEST_CHECK_NEAR(sc.offset_y(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(simulate_reports_error_when_target_cannot_be_hit) {
    // 空容器：中心处既无命中的后代、自身也不是点击目标 → 派发前即判定，返回错误而非静默成功。
    Column empty;

    const Result<void> click = Inspector::simulate_click(empty);
    AURORA_TEST_REQUIRE_FALSE(click.ok());
    AURORA_TEST_CHECK_EQ(click.error().code_enum, aurora::ErrorCode::GeneralNotSupported);

    const Result<void> scroll = Inspector::simulate_scroll(empty, 0.0F, -10.0F);
    AURORA_TEST_REQUIRE_FALSE(scroll.ok());
    AURORA_TEST_CHECK_EQ(scroll.error().code_enum, aurora::ErrorCode::GeneralNotSupported);

    // 空文本片段无副作用，直接视为完成（不派发、不报错）。
    AURORA_TEST_CHECK_TRUE(Inspector::simulate_text_input(empty, "").ok());
}

AURORA_TEST_CASE(simulate_text_input_reports_error_when_target_rejects_input) {
    // 禁用态控件不消费文本输入（on_text_input 提前返回且不置 handled）→ 派发器返回未处理。
    TextInput disabled;
    disabled.set_enabled(false);
    LayoutEngine::layout(disabled, bounded(200.0F, 40.0F));

    const Result<void> r = Inspector::simulate_text_input(disabled, "x");
    AURORA_TEST_REQUIRE_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, aurora::ErrorCode::GeneralNotSupported);
    AURORA_TEST_CHECK_EQ(disabled.value(), std::string{});
}

}  // namespace aurora::test_cases::utest_inspector_api
