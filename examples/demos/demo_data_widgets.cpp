// DataTable / TreeView / ListView 数据展示控件 demo。
#include "demo_common.h"

auto main() -> int {
    // DataTable：可排序表格
    std::vector<au::DataColumn> cols;
    cols.push_back(au::DataColumn{ .label = "姓名", .width = 100.0f, .sortable = true });
    cols.push_back(au::DataColumn{ .label = "年龄", .width = 60.0f, .sortable = true });
    au::DataTable table{ std::move(cols), { { "Alice", "30" }, { "Bob", "25" }, { "Carol", "35" } } };
    table.set_on_select([](int r) -> void { AURORA_LOG_INFO("demo", "选中行: ", r); });

    // TreeView：层级树
    au::TreeItem src;
    src.label = "src";
    src.expanded = true;
    au::TreeItem widgets;
    widgets.label = "widgets";
    widgets.children.push_back(au::TreeItem{ .label = "button.h" });
    widgets.children.push_back(au::TreeItem{ .label = "text.h" });
    src.children.push_back(std::move(widgets));
    src.children.push_back(au::TreeItem{ .label = "main.cpp" });
    std::vector<au::TreeItem> roots;
    roots.push_back(std::move(src));
    au::TreeView tree{ std::move(roots) };

    // ListView：多选列表
    au::ListView list{ std::vector<std::string>{ "Alpha", "Beta", "Gamma" }, true };

    au::Node root = au::Column{
        GradientTitle{ "DataTable / TreeView / ListView" }, gap(12), std::move(table), gap(12),
        au::Row{ std::move(tree), std::move(list) },
    };
    return run_demo(std::move(root), "DataWidgets · Aurora Demo", 560.0f, 480.0f);
}
