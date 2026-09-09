/// 测试类型: unit
/// 目标单元: include/aurora/widget/inspector_panel.h
/// 测试说明: 覆盖 InspectorPanel 类型契约/自描述/ratio 钳制与序列化、树点击选中与属性面板联动、
/// 分隔条拖拽、Export Code 导出与回调、set_root 重定向，及随附 inspect.h 的树转储/路径定位/属性读写

#include <memory>
#include <string>
#include <type_traits>

#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/inspector_panel.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_inspector_panel {

namespace {

/// @brief 构造确定性测试树：Column → [Button("OK"), Text("label"), Row → [Text("inner"), TextInput]]。
/// btn / input 由调用方持有 shared_ptr，便于断言「选中的 Widget 指针」与树内实例同一。
[[nodiscard]] auto make_sample_tree(std::shared_ptr<Button> btn, std::shared_ptr<TextInput> input) -> Node {
    auto row = std::make_shared<Row>();
    row->add(Node{Text{"inner"}});
    row->add(Node{std::move(input)});
    auto col = std::make_shared<Column>();
    col->add(Node{std::move(btn)});
    col->add(Node{Text{"label"}});
    col->add(Node{std::move(row)});
    return Node{std::move(col)};
}

/// @brief 有限约束：on_layout 取 c.max 为总尺寸，得到确定的 400x300 面板几何。
[[nodiscard]] auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(inspector_panel_type_contract_and_describe) {
    static_assert(std::is_base_of_v<aurora::Widget, aurora::InspectorPanel>);
    static_assert(std::is_base_of_v<aurora::Container, aurora::InspectorPanel>);
    static_assert(!std::is_copy_constructible_v<aurora::InspectorPanel>);

    const InspectorPanel panel;
    AURORA_TEST_CHECK_EQ(std::string{panel.type_name()}, "InspectorPanel");

    const auto d = InspectorPanel::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "InspectorPanel");
    AURORA_TEST_CHECK_EQ(std::string{panel.describe().name}, "InspectorPanel");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 1U);
    AURORA_TEST_CHECK_EQ(d.properties[0].name, "ratio");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
}

AURORA_TEST_CASE(inspector_panel_ratio_clamped_and_serialized) {
    // 构造期把 tree_ratio 钳制到 [0.1, 0.9]（不变量 ratio ∈ [0,1]），并经 serialize_props 暴露。
    const InspectorPanel low{[] { return Node{}; }, 0.05F};
    Json pj;
    low.serialize_props(pj);
    AURORA_TEST_CHECK_NEAR(pj["ratio"].get<float>(), 0.1F, 1e-4F);

    const InspectorPanel high{[] { return Node{}; }, 2.0F};
    Json pj2;
    high.serialize_props(pj2);
    AURORA_TEST_CHECK_NEAR(pj2["ratio"].get<float>(), 0.9F, 1e-4F);

    const InspectorPanel mid{[] { return Node{}; }};
    Json pj3;
    mid.serialize_props(pj3);
    AURORA_TEST_CHECK_NEAR(pj3["ratio"].get<float>(), 0.35F, 1e-4F);
}

AURORA_TEST_CASE(inspector_panel_null_root_degrades_gracefully) {
    InspectorPanel panel;  // 无 root_getter：空目标树
    AURORA_TEST_CHECK_NULL(panel.selected_widget());
    AURORA_TEST_CHECK_TRUE(panel.current_props().empty());
    AURORA_TEST_CHECK_TRUE(panel.export_code().empty());
    AURORA_TEST_CHECK_NO_THROW(panel.refresh());  // 无 getter 时刷新为 no-op，不崩溃
}

AURORA_TEST_CASE(inspector_panel_tree_click_selects_widget_and_updates_props) {
    auto btn = std::make_shared<Button>("OK");
    auto input = std::make_shared<TextInput>();
    input->set_value("hello");
    InspectorPanel panel{[&] { return make_sample_tree(btn, input); }};

    BuildContext ctx;
    const Size sz = panel.layout(bounded(400.0F, 300.0F), ctx);
    AURORA_TEST_CHECK_NEAR(sz.width, 400.0F, 1e-3F);

    int select_hits = 0;
    Widget *last_selected = nullptr;
    panel.on_select_widget = [&](Widget *w) {
        ++select_hits;
        last_selected = w;
    };
    AURORA_TEST_CHECK_NULL(panel.selected_widget());

    // 树区点击（表头高 28dp、行高 24dp）：行 0 = 根 Column，行 1 = Button。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 50.0F, .y = 52.0F};
    panel.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(panel.selected_widget(), btn.get());
    AURORA_TEST_CHECK_EQ(last_selected, btn.get());
    AURORA_TEST_CHECK_EQ(select_hits, 1);

    // 属性面板随选中更新：含 Button 的 label 属性行（值即按钮文字）。
    const auto &rows = panel.current_props();
    AURORA_TEST_CHECK_FALSE(rows.empty());
    bool has_label_ok = false;
    for (const auto &kv : rows) {
        if (kv.first == "label" && kv.second == "OK") {
            has_label_ok = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_label_ok);

    // 点击表头区域（y < 28）不改变选中、不重复触发回调。
    MouseEvent header_press;
    header_press.action = MouseAction::Press;
    header_press.local_position = Point{.x = 50.0F, .y = 10.0F};
    panel.on_pointer_event(header_press);
    AURORA_TEST_CHECK_TRUE(header_press.is_handled);
    AURORA_TEST_CHECK_EQ(panel.selected_widget(), btn.get());
    AURORA_TEST_CHECK_EQ(select_hits, 1);
}

AURORA_TEST_CASE(inspector_panel_divider_drag_updates_ratio) {
    InspectorPanel panel{[] { return Node{}; }};
    BuildContext ctx;
    (void)panel.layout(bounded(400.0F, 300.0F), ctx);

    // 按下分隔条：tree_w = 400 * 0.35 = 140，拖把区间 [140, 145]。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 142.0F, .y = 100.0F};
    panel.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    // 拖到 x = 200 → ratio = 200 / 400 = 0.5。
    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = 200.0F, .y = 100.0F};
    panel.on_pointer_event(move);
    AURORA_TEST_CHECK_TRUE(move.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    release.local_position = Point{.x = 200.0F, .y = 100.0F};
    panel.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);

    Json pj;
    panel.serialize_props(pj);
    AURORA_TEST_CHECK_NEAR(pj["ratio"].get<float>(), 0.5F, 1e-3F);
}

AURORA_TEST_CASE(inspector_panel_export_code_button_invokes_callback) {
    InspectorPanel panel{[] { return Node{Column{Node{Text{"solo"}}}}; }};

    // 直接导出：非空且含根类型名。
    const std::string direct = panel.export_code();
    AURORA_TEST_CHECK_TRUE(direct.find("Column") != std::string::npos);

    int export_hits = 0;
    std::string exported;
    panel.on_export_code = [&](const std::string &code) {
        ++export_hits;
        exported = code;
    };

    // 绘制一帧以生成 Export Code 按钮命中区（400 宽、ratio 0.35 → 按钮约 x∈[316,396]、y∈[3,25]），
    // 随后点击按钮区域触发 on_export_code。
    Painter p;
    p.begin(400, 300);
    const Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = 300.0F}};
    BuildContext ctx;
    panel.paint(p, bounds, ctx);

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 356.0F, .y = 14.0F};
    panel.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(export_hits, 1);
    AURORA_TEST_CHECK_FALSE(exported.empty());
}

AURORA_TEST_CASE(inspector_panel_set_root_retargets_selection) {
    InspectorPanel panel{[] { return Node{Column{Node{Text{"first"}}}}; }};
    BuildContext ctx;
    (void)panel.layout(bounded(400.0F, 300.0F), ctx);

    auto second_btn = std::make_shared<Button>("second");
    panel.set_root([&second_btn]() -> Node { return Node{Column{Node{second_btn}}}; });
    (void)panel.layout(bounded(400.0F, 300.0F), ctx);  // set_root 后重排以重建树映射

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 50.0F, .y = 52.0F};  // 行 1 = 新树的 Button
    panel.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(panel.selected_widget(), second_btn.get());
}

AURORA_TEST_CASE(inspector_panel_widget_tree_to_items_maps_structure) {
    auto input = std::make_shared<TextInput>();
    input->set_value("hello");
    const Node root = make_sample_tree(std::make_shared<Button>("OK"), std::move(input));

    const auto items = widget_tree_to_items(root);
    AURORA_TEST_REQUIRE_EQ(items.size(), 1U);
    AURORA_TEST_CHECK_EQ(items[0].label, "Column");
    AURORA_TEST_CHECK_TRUE(items[0].expanded);  // 根默认展开
    AURORA_TEST_REQUIRE_EQ(items[0].children.size(), 3U);
    AURORA_TEST_CHECK_EQ(items[0].children[0].label, "Button");
    AURORA_TEST_CHECK_EQ(items[0].children[1].label, "Text");
    AURORA_TEST_CHECK_EQ(items[0].children[2].label, "Row");
    AURORA_TEST_REQUIRE_EQ(items[0].children[2].children.size(), 2U);
    AURORA_TEST_CHECK_EQ(items[0].children[2].children[0].label, "Text");
    AURORA_TEST_CHECK_EQ(items[0].children[2].children[1].label, "TextInput");
    AURORA_TEST_CHECK_FALSE(items[0].children[2].expanded);  // 非根节点默认折叠
}

AURORA_TEST_CASE(inspector_panel_widget_tree_to_items_single_leaf) {
    const auto items = widget_tree_to_items(Node{Text{"solo"}});
    AURORA_TEST_REQUIRE_EQ(items.size(), 1U);
    AURORA_TEST_CHECK_EQ(items[0].label, "Text");
    AURORA_TEST_CHECK_TRUE(items[0].children.empty());
}

AURORA_TEST_CASE(inspector_panel_dump_tree_json_full_includes_props) {
    const Node root = make_sample_tree(std::make_shared<Button>("OK"), std::make_shared<TextInput>());

    const Json j = dump_tree_json_full(root);
    AURORA_TEST_CHECK_EQ(j["type"], "Column");
    AURORA_TEST_CHECK_TRUE(j["props"].is_object());
    AURORA_TEST_REQUIRE_EQ(j["children"].size(), 3U);
    AURORA_TEST_CHECK_EQ(j["children"][0]["type"], "Button");
    AURORA_TEST_CHECK_TRUE(j["children"][0].contains("props"));
    AURORA_TEST_CHECK_EQ(j["children"][0]["children"].size(), 0U);  // 叶节点 children 为空数组
}

AURORA_TEST_CASE(inspector_panel_find_node_by_path) {
    const Node root = make_sample_tree(std::make_shared<Button>("OK"), std::make_shared<TextInput>());

    const Node n0 = find_node_by_path(root, "");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(n0));
    AURORA_TEST_CHECK_EQ(std::string{n0.widget().type_name()}, "Column");

    const Node n1 = find_node_by_path(root, "0");
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(n1));
    AURORA_TEST_CHECK_EQ(std::string{n1.widget().type_name()}, "Button");

    const Node n3 = find_node_by_path(root, "2/1");  // Row 下的 TextInput
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(n3));
    AURORA_TEST_CHECK_EQ(std::string{n3.widget().type_name()}, "TextInput");

    // 越界路径返回空 Node。
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(find_node_by_path(root, "99")));
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(find_node_by_path(root, "2/5")));
}

AURORA_TEST_CASE(inspector_panel_get_and_set_widget_props) {
    auto col = std::make_shared<Column>();
    col->set_gap(8.0F);
    col->add(Node{Text{"a"}});
    Node root{std::move(col)};

    const Json props = get_widget_props(root.widget());
    AURORA_TEST_CHECK_EQ(props["descriptor"]["name"], "Column");
    AURORA_TEST_CHECK_TRUE(props["values"].is_object());
    AURORA_TEST_CHECK_TRUE(props["values"].contains("gap"));
    AURORA_TEST_CHECK_NEAR(props["values"]["gap"].get<float>(), 8.0F, 1e-4F);

    // 单属性回写 → 读回一致；再恢复原值。
    auto &col_ref = dynamic_cast<Column &>(root.widget());
    set_widget_prop(root.widget(), "gap", Json(16.0F));
    AURORA_TEST_CHECK_NEAR(col_ref.gap, 16.0F, 1e-4F);
    set_widget_prop(root.widget(), "gap", Json(8.0F));
    AURORA_TEST_CHECK_NEAR(col_ref.gap, 8.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_inspector_panel
