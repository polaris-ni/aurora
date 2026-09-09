/// 测试类型: unit
/// 目标单元: include/aurora/widget/data_widgets.h
/// 测试说明: 覆盖 DataTable/TreeView/ListView
/// 的构造与单元格访问、选中与排序回调、越界防御、指针命中、布局尺寸、序列化与自描述，含离屏绘制冒烟

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/data_widgets.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_data_widgets {

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto make_table() -> DataTable {
    std::vector<DataColumn> cols;
    cols.push_back(DataColumn{.label = "Name", .width = 120.0F, .sortable = true});
    cols.push_back(DataColumn{.label = "Age", .width = 60.0F, .sortable = false});
    std::vector<std::vector<std::string>> rows = {{"Alice", "30"}, {"Bob", "25"}, {"Carol", "35"}};
    return DataTable{std::move(cols), std::move(rows)};
}

auto make_tree() -> TreeView {
    TreeItem src;
    src.label = "src";
    src.expanded = true;
    TreeItem widgets;
    widgets.label = "widgets";
    widgets.children.push_back(TreeItem{.label = "button.h"});
    widgets.children.push_back(TreeItem{.label = "text.h"});
    src.children.push_back(std::move(widgets));
    src.children.push_back(TreeItem{.label = "main.cpp"});
    std::vector<TreeItem> roots;
    roots.push_back(std::move(src));
    roots.push_back(TreeItem{.label = "README"});
    return TreeView{std::move(roots)};
}

}  // namespace

AURORA_TEST_CASE(data_widgets_type_contract) {
    static_assert(std::is_base_of_v<aurora::Widget, DataTable>);
    static_assert(std::is_base_of_v<aurora::Widget, TreeView>);
    static_assert(std::is_base_of_v<aurora::Widget, ListView>);
    static_assert(!std::is_copy_constructible_v<DataTable>);  // Widget 拷贝被删除
    static_assert(!std::is_copy_constructible_v<TreeView>);
    static_assert(!std::is_copy_constructible_v<ListView>);

    AURORA_TEST_CHECK_EQ(std::string{DataTable{}.type_name()}, "DataTable");
    AURORA_TEST_CHECK_EQ(std::string{TreeView{}.type_name()}, "TreeView");
    AURORA_TEST_CHECK_EQ(std::string{ListView{}.type_name()}, "ListView");

    // 三者均为点击目标（自带 on_pointer_event 处理）
    AURORA_TEST_CHECK_TRUE(make_table().wants_click());
    AURORA_TEST_CHECK_TRUE(make_tree().wants_click());
    AURORA_TEST_CHECK_TRUE(ListView{}.wants_click());
}

AURORA_TEST_CASE(data_table_construction_and_cell_access) {
    const DataTable dt = make_table();
    AURORA_TEST_CHECK_EQ(dt.column_count(), 2U);
    AURORA_TEST_CHECK_EQ(dt.row_count(), 3U);
    AURORA_TEST_CHECK_EQ(dt.cell(0, 0), std::string{"Alice"});
    AURORA_TEST_CHECK_EQ(dt.cell(2, 1), std::string{"35"});
    AURORA_TEST_CHECK_TRUE(dt.cell(9, 9).empty());  // 行越界 → 空串
    AURORA_TEST_CHECK_TRUE(dt.cell(0, 9).empty());  // 列越界 → 空串
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), -1);  // 默认未选中
    AURORA_TEST_CHECK_EQ(dt.sort_column(), -1);
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::None);

    const DataTable empty;
    AURORA_TEST_CHECK_EQ(empty.column_count(), 0U);
    AURORA_TEST_CHECK_EQ(empty.row_count(), 0U);
}

AURORA_TEST_CASE(data_table_select_row_bounds_and_callback) {
    DataTable dt = make_table();
    int last = -100;
    int calls = 0;
    dt.set_on_select([&last, &calls](int r) -> void {
        last = r;
        ++calls;
    });

    dt.select_row(1);
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 1);
    AURORA_TEST_CHECK_EQ(last, 1);

    dt.select_row(1);  // 同行重复：不回调
    AURORA_TEST_CHECK_EQ(calls, 1);

    dt.select_row(99);  // 越界忽略
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 1);
    AURORA_TEST_CHECK_EQ(calls, 1);

    dt.select_row(-1);  // -1 取消选中
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), -1);
    AURORA_TEST_CHECK_EQ(last, -1);
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(data_table_sort_cycle_skips_unsortable) {
    DataTable dt = make_table();
    int last_col = -100;
    SortOrder last_order = SortOrder::None;
    int calls = 0;
    dt.set_on_sort([&last_col, &last_order, &calls](int c, SortOrder o) -> void {
        last_col = c;
        last_order = o;
        ++calls;
    });

    dt.sort_by(0);  // 换列 → Ascending
    AURORA_TEST_CHECK_EQ(last_col, 0);
    AURORA_TEST_CHECK_EQ(last_order, SortOrder::Ascending);
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::Ascending);
    dt.sort_by(0);  // 同列循环 → Descending
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::Descending);
    dt.sort_by(0);  // → Ascending
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::Ascending);
    AURORA_TEST_CHECK_EQ(calls, 3);

    dt.sort_by(1);  // Age 列 sortable=false：无操作、不回调
    AURORA_TEST_CHECK_EQ(calls, 3);
    AURORA_TEST_CHECK_EQ(dt.sort_column(), 0);
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::Ascending);

    dt.sort_by(-1);  // 越界列：无操作
    dt.sort_by(5);
    AURORA_TEST_CHECK_EQ(calls, 3);
}

AURORA_TEST_CASE(data_table_set_rows_resets_stale_selection) {
    DataTable dt = make_table();
    dt.select_row(2);
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 2);

    dt.set_rows({{"OnlyOne", "1"}});
    AURORA_TEST_CHECK_EQ(dt.row_count(), 1U);
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), -1);  // 选中越界 → 重置为 -1
    AURORA_TEST_CHECK_EQ(dt.cell(0, 0), std::string{"OnlyOne"});

    dt.select_row(0);
    dt.set_rows({{"A", "1"}, {"B", "2"}, {"C", "3"}});
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 0);  // 选中仍有效则保留
    AURORA_TEST_CHECK_EQ(dt.row_count(), 3U);
}

AURORA_TEST_CASE(data_table_pointer_header_sort_and_row_select) {
    DataTable dt = make_table();
    LayoutEngine::layout(dt, bounded(400.0F, 600.0F));

    MouseEvent header;
    header.action = MouseAction::Press;
    header.local_position = Point{.x = 50.0F, .y = 16.0F};  // 表头高 32，落在第 0 列
    dt.on_pointer_event(header);
    AURORA_TEST_CHECK_TRUE(header.is_handled);
    AURORA_TEST_CHECK_EQ(dt.sort_column(), 0);
    AURORA_TEST_CHECK_EQ(dt.sort_order(), SortOrder::Ascending);

    MouseEvent row;
    row.action = MouseAction::Press;
    row.local_position = Point{.x = 50.0F, .y = 32.0F + 28.0F + 14.0F};  // 行高 28，第 1 行
    dt.on_pointer_event(row);
    AURORA_TEST_CHECK_TRUE(row.is_handled);
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 1);

    MouseEvent unsortable;
    unsortable.action = MouseAction::Press;
    unsortable.local_position = Point{.x = 150.0F, .y = 16.0F};  // 第 1 列表头不可排序
    dt.on_pointer_event(unsortable);
    AURORA_TEST_CHECK_TRUE(unsortable.is_handled);
    AURORA_TEST_CHECK_EQ(dt.sort_column(), 0);  // 排序列不变
}

AURORA_TEST_CASE(data_table_layout_size_matches_content) {
    DataTable dt = make_table();
    LayoutEngine::layout(dt, bounded(400.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(dt.size().width, 180.0F, 1e-3F);  // 列宽之和 120 + 60
    AURORA_TEST_CHECK_NEAR(dt.size().height, 116.0F, 1e-3F);  // 表头 32 + 3 行 × 28

    DataTable empty;
    LayoutEngine::layout(empty, bounded(400.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(empty.size().height, 32.0F, 1e-3F);  // 仅表头
}

AURORA_TEST_CASE(data_table_signal_view_write_through) {
    DataTable dt = make_table();
    std::vector<SignalViewBase*> sigs;
    dt.collect_signals(sigs);
    AURORA_TEST_REQUIRE_EQ(sigs.size(), 1U);  // selected_row 信号入收集集

    dt.selected_row().set(2);  // 信号直写 → 索引随之变化
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), 2);
    dt.selected_row().set(-1);
    AURORA_TEST_CHECK_EQ(dt.selected_row_index(), -1);
}

AURORA_TEST_CASE(data_table_serialize_props) {
    DataTable dt = make_table();
    dt.select_row(1);
    dt.sort_by(0);

    Json props;
    dt.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["row_count"].get<int>(), 3);
    AURORA_TEST_CHECK_EQ(props["selected_row"].get<int>(), 1);
    AURORA_TEST_CHECK_EQ(props["sort_column"].get<int>(), 0);
    AURORA_TEST_REQUIRE_EQ(props["columns"].size(), 2U);
    AURORA_TEST_CHECK_EQ(props["columns"][0]["label"].get<std::string>(), std::string{"Name"});
    AURORA_TEST_CHECK_EQ(props["columns"][0]["sortable"].get<bool>(), true);
    AURORA_TEST_CHECK_EQ(props["columns"][1]["sortable"].get<bool>(), false);
    AURORA_TEST_CHECK_EQ(props["show"].get<bool>(), true);  // 基类通用属性保留
}

AURORA_TEST_CASE(data_table_describe_reports_invariants) {
    const auto d = DataTable::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "DataTable");
    AURORA_TEST_CHECK_EQ(d.children_policy, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 2U);
    AURORA_TEST_CHECK_EQ(d.events[0], "on_sort");
    AURORA_TEST_CHECK_EQ(d.events[1], "on_select");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 4U);
    AURORA_TEST_CHECK_EQ(d.properties[0].name, "columns");
    AURORA_TEST_CHECK_TRUE(d.properties[0].required);
    AURORA_TEST_CHECK_EQ(d.invariants[0], "row_count >= 0");
}

AURORA_TEST_CASE(tree_view_visible_order_and_depth) {
    const TreeView tv = make_tree();
    // 可见序：src(展开) → widgets(折叠) → main.cpp → README
    AURORA_TEST_CHECK_EQ(tv.visible_count(), 4U);
    AURORA_TEST_CHECK_EQ(tv.visible_label(0), std::string{"src"});
    AURORA_TEST_CHECK_EQ(tv.visible_label(1), std::string{"widgets"});
    AURORA_TEST_CHECK_EQ(tv.visible_label(2), std::string{"main.cpp"});
    AURORA_TEST_CHECK_EQ(tv.visible_label(3), std::string{"README"});
    AURORA_TEST_CHECK_TRUE(tv.visible_label(9).empty());  // 越界 → 空串
    AURORA_TEST_CHECK_EQ(tv.visible_depth(0), 0);
    AURORA_TEST_CHECK_EQ(tv.visible_depth(1), 1);
    AURORA_TEST_CHECK_EQ(tv.visible_depth(2), 1);  // main.cpp 与 widgets 同层
    AURORA_TEST_CHECK_EQ(tv.visible_depth(3), 0);
    AURORA_TEST_CHECK_EQ(tv.visible_depth(99), 0);  // 越界回退 0
}

AURORA_TEST_CASE(tree_view_toggle_expand_collapse_leaf_noop) {
    TreeView tv = make_tree();
    int last_row = -100;
    bool last_open = false;
    tv.set_on_toggle([&last_row, &last_open](int r, bool open) -> void {
        last_row = r;
        last_open = open;
    });

    tv.toggle(1);  // 展开 widgets
    AURORA_TEST_CHECK_EQ(last_row, 1);
    AURORA_TEST_CHECK_TRUE(last_open);
    AURORA_TEST_CHECK_EQ(tv.visible_count(), 6U);
    AURORA_TEST_CHECK_EQ(tv.visible_label(2), std::string{"button.h"});
    AURORA_TEST_CHECK_EQ(tv.visible_depth(2), 2);
    AURORA_TEST_CHECK_EQ(tv.visible_depth(3), 2);  // text.h 与 button.h 同层
    AURORA_TEST_CHECK_EQ(tv.visible_label(4), std::string{"main.cpp"});
    AURORA_TEST_CHECK_EQ(tv.visible_depth(4), 1);
    AURORA_TEST_CHECK_EQ(tv.visible_depth(5), 0);  // README

    tv.toggle(2);  // 叶节点 toggle 无操作
    AURORA_TEST_CHECK_EQ(tv.visible_count(), 6U);
    AURORA_TEST_CHECK_EQ(tv.visible_label(2), std::string{"button.h"});

    tv.toggle(1);  // 折叠回去
    AURORA_TEST_CHECK_EQ(tv.visible_count(), 4U);
    AURORA_TEST_CHECK_FALSE(last_open);
}

AURORA_TEST_CASE(tree_view_select_bounds_and_callback) {
    TreeView tv = make_tree();
    int last = -100;
    int calls = 0;
    tv.set_on_select([&last, &calls](int r) -> void {
        last = r;
        ++calls;
    });

    tv.select(3);
    AURORA_TEST_CHECK_EQ(tv.selected_row(), 3);
    AURORA_TEST_CHECK_EQ(last, 3);

    tv.select(99);  // 越界忽略、不回调
    AURORA_TEST_CHECK_EQ(calls, 1);

    tv.select(3);  // 同行不重复回调
    AURORA_TEST_CHECK_EQ(calls, 1);

    tv.select(-1);  // 取消选中
    AURORA_TEST_CHECK_EQ(tv.selected_row(), -1);
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(tree_view_pointer_arrow_toggles_text_selects) {
    TreeView tv = make_tree();
    LayoutEngine::layout(tv, bounded(300.0F, 200.0F));

    // 第 1 行 widgets（行高 26，深度 1 → 缩进 18，箭头区 [18,38)）：x=20 命中箭头 → 展开
    MouseEvent arrow;
    arrow.action = MouseAction::Press;
    arrow.local_position = Point{.x = 20.0F, .y = 26.0F + 13.0F};
    tv.on_pointer_event(arrow);
    AURORA_TEST_CHECK_TRUE(arrow.is_handled);
    AURORA_TEST_CHECK_EQ(tv.visible_count(), 6U);

    // 第 3 行 README（深度 0，箭头区 [0,20)）：x=30 落在文本区 → 选中
    MouseEvent text;
    text.action = MouseAction::Press;
    text.local_position = Point{.x = 30.0F, .y = (26.0F * 3.0F) + 13.0F};
    tv.on_pointer_event(text);
    AURORA_TEST_CHECK_EQ(tv.selected_row(), 3);
}

AURORA_TEST_CASE(tree_view_serialize_props) {
    TreeView tv = make_tree();
    tv.select(2);

    Json props;
    tv.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["selected_row"].get<int>(), 2);
    AURORA_TEST_CHECK_EQ(props["visible_count"].get<int>(), 4);
    AURORA_TEST_CHECK_EQ(props["show"].get<bool>(), true);
}

AURORA_TEST_CASE(list_view_single_select_replaces) {
    ListView lv{std::vector<std::string>{"A", "B", "C"}};
    AURORA_TEST_CHECK_EQ(lv.item_count(), 3U);
    AURORA_TEST_CHECK_EQ(lv.item(1), std::string{"B"});
    AURORA_TEST_CHECK_TRUE(lv.item(9).empty());  // 越界 → 空串
    AURORA_TEST_CHECK_TRUE(lv.selection().empty());  // 默认无选中

    lv.select(1);
    AURORA_TEST_CHECK_TRUE(lv.is_selected(1));
    lv.select(2);  // 单选替换
    AURORA_TEST_CHECK_FALSE(lv.is_selected(1));
    AURORA_TEST_CHECK_TRUE(lv.is_selected(2));
    AURORA_TEST_REQUIRE_EQ(lv.selection().size(), 1U);

    lv.select(99);  // 越界忽略
    AURORA_TEST_CHECK_EQ(lv.selection().size(), 1U);
    lv.select(-1);  // 负行号忽略
    AURORA_TEST_CHECK_EQ(lv.selection().size(), 1U);
}

AURORA_TEST_CASE(list_view_multi_select_toggle_and_clear) {
    ListView lv{std::vector<std::string>{"A", "B", "C"}, true};
    lv.select(2);
    lv.select(0);  // 多选追加，升序 {0,2}
    AURORA_TEST_REQUIRE_EQ(lv.selection().size(), 2U);
    AURORA_TEST_CHECK_TRUE(lv.is_selected(0));
    AURORA_TEST_CHECK_TRUE(lv.is_selected(2));

    lv.select(0);  // 再点取消
    AURORA_TEST_CHECK_FALSE(lv.is_selected(0));
    AURORA_TEST_REQUIRE_EQ(lv.selection().size(), 1U);

    lv.clear_selection();
    AURORA_TEST_CHECK_TRUE(lv.selection().empty());
}

AURORA_TEST_CASE(list_view_remove_fixes_selection_and_append) {
    ListView lv{std::vector<std::string>{"A", "B", "C", "D"}, true};
    lv.select(1);
    lv.select(3);
    int removed = -100;
    lv.set_on_remove([&removed](int r) -> void { removed = r; });

    lv.remove(1);  // 删除 B：选中 {1,3} → {2}（D 前移）
    AURORA_TEST_CHECK_EQ(removed, 1);
    AURORA_TEST_CHECK_EQ(lv.item_count(), 3U);
    AURORA_TEST_CHECK_EQ(lv.item(1), std::string{"C"});
    AURORA_TEST_REQUIRE_EQ(lv.selection().size(), 1U);
    AURORA_TEST_CHECK_EQ(lv.selection()[0], 2);

    lv.remove(99);  // 越界：无操作不回调
    AURORA_TEST_CHECK_EQ(removed, 1);
    AURORA_TEST_CHECK_EQ(lv.item_count(), 3U);

    lv.append("E");
    AURORA_TEST_CHECK_EQ(lv.item_count(), 4U);
    AURORA_TEST_CHECK_EQ(lv.item(3), std::string{"E"});
}

AURORA_TEST_CASE(list_view_pointer_row_select) {
    ListView lv{std::vector<std::string>{"X", "Y"}};
    LayoutEngine::layout(lv, bounded(400.0F, 600.0F));
    AURORA_TEST_CHECK_NEAR(lv.size().width, 400.0F, 1e-3F);  // 宽取约束上限
    AURORA_TEST_CHECK_NEAR(lv.size().height, 52.0F, 1e-3F);  // 2 行 × 26

    MouseEvent row;
    row.action = MouseAction::Press;
    row.local_position = Point{.x = 20.0F, .y = 26.0F + 13.0F};  // 第 1 行
    lv.on_pointer_event(row);
    AURORA_TEST_CHECK_TRUE(row.is_handled);
    AURORA_TEST_CHECK_TRUE(lv.is_selected(1));

    MouseEvent beyond;
    beyond.action = MouseAction::Press;
    beyond.local_position = Point{.x = 20.0F, .y = 300.0F};  // 行号越界：消费但选中不变
    lv.on_pointer_event(beyond);
    AURORA_TEST_CHECK_TRUE(beyond.is_handled);
    AURORA_TEST_CHECK_TRUE(lv.is_selected(1));
    AURORA_TEST_CHECK_FALSE(lv.is_selected(0));
}

AURORA_TEST_CASE(list_view_serialize_deserialize_roundtrip) {
    ListView src{std::vector<std::string>{"X", "Y"}, true};
    src.select(1);  // 选中态不参与序列化

    Json props;
    src.serialize_props(props);
    AURORA_TEST_REQUIRE_EQ(props["items"].size(), 2U);
    AURORA_TEST_CHECK_EQ(props["items"][0].get<std::string>(), std::string{"X"});
    AURORA_TEST_CHECK_EQ(props["multi_select"].get<bool>(), true);

    ListView dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.item_count(), 2U);
    AURORA_TEST_CHECK_EQ(dst.item(0), std::string{"X"});
    dst.select(0);
    dst.select(1);
    AURORA_TEST_REQUIRE_EQ(dst.selection().size(), 2U);  // multi_select 已还原为多选
}

AURORA_TEST_CASE(list_view_describe_and_signals) {
    const auto d = ListView::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "ListView");
    AURORA_TEST_CHECK_EQ(d.children_policy, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 2U);
    AURORA_TEST_CHECK_EQ(d.events[0], "on_select");
    AURORA_TEST_CHECK_EQ(d.events[1], "on_remove");
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 2U);
    AURORA_TEST_CHECK_EQ(d.properties[0].name, "items");
    AURORA_TEST_CHECK_TRUE(d.properties[0].required);

    ListView lv;
    std::vector<SignalViewBase*> sigs;
    lv.collect_signals(sigs);
    AURORA_TEST_CHECK_THAT(sigs, aurora::testing::matchers::is_empty());  // 无内部信号
}

AURORA_TEST_CASE(data_widgets_offscreen_paint_smoke) {
    DataTable dt = make_table();
    TreeView tv = make_tree();
    ListView lv{std::vector<std::string>{"X", "Y"}};
    LayoutEngine::layout(dt, bounded(180.0F, 300.0F));
    LayoutEngine::layout(tv, bounded(200.0F, 300.0F));
    LayoutEngine::layout(lv, bounded(200.0F, 300.0F));

    Painter p;
    p.begin(240, 300);
    const BuildContext ctx;
    dt.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 180.0F, .height = 116.0F}}, ctx);
    tv.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 120.0F}, .size = Size{.width = 104.0F, .height = 104.0F}}, ctx);
    lv.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 230.0F}, .size = Size{.width = 200.0F, .height = 52.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p.width(), 240);
    AURORA_TEST_CHECK_EQ(p.height(), 300);
}

}  // namespace aurora::test_cases::utest_data_widgets
