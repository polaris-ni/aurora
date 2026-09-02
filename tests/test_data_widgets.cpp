// 验证数据展示控件：DataTable（排序/选中/单元格）、TreeView（展开/可见序/深度）、
// ListView（单选/多选/删除修正）。

#include <cstdio>
#include <memory>

#include "aurora/widget/data_widgets.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Constraints;
using aurora::DataColumn;
using aurora::DataTable;
using aurora::ListView;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::SortOrder;
using aurora::TreeItem;
using aurora::TreeView;

namespace {

template<typename W> auto layout_widget(std::shared_ptr<W> &w) -> void {
    BuildContext ctx;
    w->mount(ctx);
    Constraints c;
    c.min = Size{ .width = 0.0f, .height = 0.0f };
    c.max = Size{ .width = 400.0f, .height = 600.0f };
    w->layout(c, ctx);
}

auto make_table() -> std::shared_ptr<DataTable> {
    std::vector<DataColumn> cols;
    cols.push_back(DataColumn{ .label = "Name", .width = 120.0f, .sortable = true });
    cols.push_back(DataColumn{ .label = "Age", .width = 60.0f, .sortable = false });
    std::vector<std::vector<std::string>> rows = { { "Alice", "30" }, { "Bob", "25" }, { "Carol", "35" } };
    return std::make_shared<DataTable>(std::move(cols), std::move(rows));
}

auto make_tree() -> std::shared_ptr<TreeView> {
    TreeItem root;
    root.label = "src";
    root.expanded = true;
    TreeItem sub;
    sub.label = "widgets";
    sub.children.push_back(TreeItem{ .label = "button.h" });
    sub.children.push_back(TreeItem{ .label = "text.h" });
    root.children.push_back(sub);
    root.children.push_back(TreeItem{ .label = "main.cpp" });
    std::vector<TreeItem> roots;
    roots.push_back(std::move(root));
    roots.push_back(TreeItem{ .label = "README" });
    return std::make_shared<TreeView>(std::move(roots));
}

} // namespace

AURORA_TEST() {
    // ==================== DataTable ====================

    // ---- 1. 构造与单元格访问 ----
    {
        auto dt = make_table();
        AURORA_TEST_CHECK(dt->column_count() == 2);
        AURORA_TEST_CHECK(dt->row_count() == 3);
        AURORA_TEST_CHECK(dt->cell(0, 0) == "Alice");
        AURORA_TEST_CHECK(dt->cell(2, 1) == "35");
        AURORA_TEST_CHECK(dt->cell(9, 9).empty());
        AURORA_TEST_CHECK(dt->selected_row_index() == -1);
    }

    // ---- 2. 行选中与回调 ----
    {
        auto dt = make_table();
        int selected = -2;
        dt->set_on_select([&selected](int r) -> void { selected = r; });

        dt->select_row(1);
        AURORA_TEST_CHECK(dt->selected_row_index() == 1);
        AURORA_TEST_CHECK(selected == 1);

        dt->select_row(99); // 越界忽略
        AURORA_TEST_CHECK(dt->selected_row_index() == 1);

        dt->select_row(-1); // 取消选中
        AURORA_TEST_CHECK(dt->selected_row_index() == -1);
    }

    // ---- 3. 排序循环与不可排序列 ----
    {
        auto dt = make_table();
        int sort_col = -1;
        auto sort_ord = SortOrder::None;
        dt->set_on_sort([&](int c, SortOrder o) -> void {
            sort_col = c;
            sort_ord = o;
        });

        dt->sort_by(0); // Asc
        AURORA_TEST_CHECK(sort_col == 0 && sort_ord == SortOrder::Ascending);
        dt->sort_by(0); // Desc
        AURORA_TEST_CHECK(sort_ord == SortOrder::Descending);
        dt->sort_by(0); // Asc
        AURORA_TEST_CHECK(sort_ord == SortOrder::Ascending);

        // Age 列不可排序
        sort_col = -1;
        dt->sort_by(1);
        AURORA_TEST_CHECK(sort_col == -1);
        AURORA_TEST_CHECK(dt->sort_column() == 0);
    }

    // ---- 4. set_rows 数据刷新与选中修正 ----
    {
        auto dt = make_table();
        dt->select_row(2);
        dt->set_rows({ { "OnlyOne", "1" } });
        AURORA_TEST_CHECK(dt->row_count() == 1);
        AURORA_TEST_CHECK(dt->selected_row_index() == -1); // 越界重置
    }

    // ---- 5. 点击交互：表头排序 + 行选中（表头高 32，行高 28）----
    {
        auto dt = make_table();
        layout_widget(dt);

        // 点第一列表头（可排序）
        MouseEvent e1;
        e1.action = MouseAction::Press;
        e1.local_position = Point{ .x = 50.0f, .y = 16.0f };
        dt->on_pointer_event(e1);
        AURORA_TEST_CHECK(dt->sort_column() == 0);

        // 点第二行数据
        MouseEvent e2;
        e2.action = MouseAction::Press;
        e2.local_position = Point{ .x = 50.0f, .y = 32.0f + 28.0f + 14.0f };
        dt->on_pointer_event(e2);
        AURORA_TEST_CHECK(dt->selected_row_index() == 1);
    }

    // ==================== TreeView ====================

    // ---- 6. 可见序（root 展开、widgets 折叠）----
    {
        auto tv = make_tree();
        // 可见：src(展开) -> widgets(折叠) + main.cpp；README
        AURORA_TEST_CHECK(tv->visible_count() == 4);
        AURORA_TEST_CHECK(tv->visible_label(0) == "src");
        AURORA_TEST_CHECK(tv->visible_label(1) == "widgets");
        AURORA_TEST_CHECK(tv->visible_label(2) == "main.cpp");
        AURORA_TEST_CHECK(tv->visible_label(3) == "README");
        AURORA_TEST_CHECK(tv->visible_label(9).empty());
    }

    // ---- 7. 展开/折叠与深度 ----
    {
        auto tv = make_tree();
        bool toggled_state = false;
        tv->set_on_toggle([&](int, bool open) -> void { toggled_state = open; });

        tv->toggle(1); // 展开 widgets
        AURORA_TEST_CHECK(toggled_state);
        AURORA_TEST_CHECK(tv->visible_count() == 6);
        AURORA_TEST_CHECK(tv->visible_label(2) == "button.h");
        AURORA_TEST_CHECK(tv->visible_depth(0) == 0);
        AURORA_TEST_CHECK(tv->visible_depth(1) == 1);
        AURORA_TEST_CHECK(tv->visible_depth(2) == 2);

        // 叶节点 toggle 无操作
        tv->toggle(2);
        AURORA_TEST_CHECK(tv->visible_count() == 6);

        // 折叠回去
        tv->toggle(1);
        AURORA_TEST_CHECK(tv->visible_count() == 4);
    }

    // ---- 8. 选中与回调 ----
    {
        auto tv = make_tree();
        int selected = -2;
        tv->set_on_select([&selected](int r) -> void { selected = r; });
        tv->select(3);
        AURORA_TEST_CHECK(tv->selected_row() == 3);
        AURORA_TEST_CHECK(selected == 3);
        tv->select(99);
        AURORA_TEST_CHECK(tv->selected_row() == 3);
    }

    // ==================== ListView ====================

    // ---- 9. 单选模式 ----
    {
        auto lv = std::make_shared<ListView>(std::vector<std::string>{ "A", "B", "C" });
        AURORA_TEST_CHECK(lv->item_count() == 3);

        lv->select(1);
        AURORA_TEST_CHECK(lv->is_selected(1));
        lv->select(2); // 单选替换
        AURORA_TEST_CHECK(!lv->is_selected(1));
        AURORA_TEST_CHECK(lv->is_selected(2));
        AURORA_TEST_CHECK(lv->selection().size() == 1);
    }

    // ---- 10. 多选切换 ----
    {
        auto lv = std::make_shared<ListView>(std::vector<std::string>{ "A", "B", "C" }, true);
        lv->select(0);
        lv->select(2);
        AURORA_TEST_CHECK(lv->selection().size() == 2);
        AURORA_TEST_CHECK(lv->is_selected(0) && lv->is_selected(2));

        lv->select(0); // 再点取消
        AURORA_TEST_CHECK(!lv->is_selected(0));
        AURORA_TEST_CHECK(lv->selection().size() == 1);

        lv->clear_selection();
        AURORA_TEST_CHECK(lv->selection().empty());
    }

    // ---- 11. 删除行修正选中 ----
    {
        auto lv = std::make_shared<ListView>(std::vector<std::string>{ "A", "B", "C", "D" }, true);
        lv->select(1);
        lv->select(3);
        int removed = -1;
        lv->set_on_remove([&removed](int r) -> void { removed = r; });

        lv->remove(1); // 删除 B：选中 {1,3} → {2}（D 前移）
        AURORA_TEST_CHECK(removed == 1);
        AURORA_TEST_CHECK(lv->item_count() == 3);
        AURORA_TEST_CHECK(lv->item(1) == "C");
        AURORA_TEST_CHECK(lv->selection().size() == 1);
        AURORA_TEST_CHECK(lv->selection()[0] == 2);

        lv->append("E");
        AURORA_TEST_CHECK(lv->item_count() == 4);
    }

    // ---- 12. 点击选中 + 渲染 + 序列化 ----
    {
        auto lv = std::make_shared<ListView>(std::vector<std::string>{ "X", "Y" });
        layout_widget(lv);

        MouseEvent e;
        e.action = MouseAction::Press;
        e.local_position = Point{ .x = 20.0f, .y = 26.0f + 13.0f }; // 第二行
        lv->on_pointer_event(e);
        AURORA_TEST_CHECK(lv->is_selected(1));

        aurora::Painter p;
        p.begin(400, 600);
        BuildContext ctx;
        auto dt = make_table();
        auto tv = make_tree();
        layout_widget(dt);
        layout_widget(tv);
        dt->paint(p, Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 180.0f, .height = 120.0f } },
                  ctx);
        tv->paint(p,
                  Rect{ .origin = Point{ .x = 0.0f, .y = 130.0f }, .size = Size{ .width = 200.0f, .height = 110.0f } },
                  ctx);
        lv->paint(p,
                  Rect{ .origin = Point{ .x = 0.0f, .y = 250.0f }, .size = Size{ .width = 200.0f, .height = 52.0f } },
                  ctx);
        AURORA_TEST_CHECK(p.width() == 400);

        aurora::Json props;
        lv->serialize_props(props);
        AURORA_TEST_CHECK(props["items"].size() == 2);

        auto lv2 = std::make_shared<ListView>();
        lv2->deserialize_props(props);
        AURORA_TEST_CHECK(lv2->item_count() == 2);
        AURORA_TEST_CHECK(lv2->item(0) == "X");
    }
}
