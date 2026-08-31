#include "aurora/widget/data_widgets.h"

#include <utility>

#include "aurora/core/diagnostics.h"

namespace aurora {

auto DataTable::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "DataTable",
        .properties = {
            { .name = "columns", .type = "vector<DataColumn>", .default_value = "[]", .required = true, .note = "列描述", .json_type = "array" },
            { .name = "row_count", .type = "int", .default_value = "0", .required = false, .note = "行数（只读）", .json_type = "integer", .enum_values = {}, .min_value = "0" },
            { .name = "selected_row", .type = "int", .default_value = "-1", .required = false, .note = "选中行(-1=无)", .json_type = "integer" },
            { .name = "sort_column", .type = "int", .default_value = "-1", .required = false, .note = "排序列(-1=无)", .json_type = "integer" },
        },
        .events = { "on_sort", "on_select" },
        .children_policy = "none",
        .invariants = { "row_count >= 0" },
        .examples = { R"(au::DataTable({{"Name", 120}, {"Age", 60}}, {{"Alice", "30"}}))" },
    };
}

auto DataTable::on_pointer_event(MouseEvent &e) -> void {
    if (e.action != MouseAction::Press) {
        Widget::on_pointer_event(e);
        return;
    }
    // 表头
    if (e.local_position.y < m_aurora_header_height) {
        float x = 0.0f;
        for (std::size_t i = 0; i < m_columns.size(); ++i) {
            if (e.local_position.x >= x && e.local_position.x < x + m_columns[i].width) {
                sort_by(static_cast<int>(i));
                break;
            }
            x += m_columns[i].width;
        }
        e.handled = true;
        return;
    }
    // 数据行
    const int row = static_cast<int>((e.local_position.y - m_aurora_header_height) / m_aurora_row_height);
    if (row >= 0 && std::cmp_less(row, m_rows.size())) {
        select_row(row);
    }
    e.handled = true;
}

auto DataTable::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    Json cols = Json::array();
    for (const auto &c : m_columns) {
        Json jc;
        jc["label"] = c.label;
        jc["width"] = c.width;
        jc["sortable"] = c.sortable;
        cols.push_back(jc);
    }
    props["columns"] = cols;
    props["row_count"] = static_cast<int>(m_rows.size());
    props["selected_row"] = m_selected_row.get();
    props["sort_column"] = m_sort_column;
}

auto DataTable::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    float w = 0.0f;
    for (const auto &col : m_columns) {
        w += col.width;
    }
    const float h = m_aurora_header_height + (static_cast<float>(m_rows.size()) * m_aurora_row_height);
    return c.constrain(Size{ .width = w, .height = h });
}

auto DataTable::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0f;
    // 表头
    const Rect head{ .origin = bounds.origin,
                     .size = Size{ .width = bounds.size.width, .height = m_aurora_header_height } };
    p.fill_rect(head, Color(246, 246, 248, 255));
    float x = bounds.origin.x;
    for (const auto &col : m_columns) {
        std::string label = col.label;
        if (m_sort_column >= 0 && &col == &m_columns[static_cast<std::size_t>(m_sort_column)]) {
            label += m_sort_order == SortOrder::Ascending ? " ^" : " v";
        }
        p.draw_text(Rect{ .origin = Point{ .x = x + 8.0f, .y = head.origin.y + 8.0f },
                          .size = Size{ .width = col.width - 12.0f, .height = m_aurora_header_height - 12.0f } },
                    label, f, Color(60, 60, 65, 255));
        x += col.width;
    }
    // 行
    const int sel = m_selected_row.get();
    for (std::size_t r = 0; r < m_rows.size(); ++r) {
        const float ry = bounds.origin.y + m_aurora_header_height + (static_cast<float>(r) * m_aurora_row_height);
        if (std::cmp_equal(r, sel)) {
            p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = ry },
                              .size = Size{ .width = bounds.size.width, .height = m_aurora_row_height } },
                        Color(0, 122, 255, 30));
        } else if (r % 2 == 1) {
            p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = ry },
                              .size = Size{ .width = bounds.size.width, .height = m_aurora_row_height } },
                        Color(249, 249, 251, 255));
        }
        float cx = bounds.origin.x;
        for (std::size_t ci = 0; ci < m_columns.size(); ++ci) {
            p.draw_text(
                Rect{ .origin = Point{ .x = cx + 8.0f, .y = ry + 6.0f },
                      .size = Size{ .width = m_columns[ci].width - 12.0f, .height = m_aurora_row_height - 10.0f } },
                cell(r, ci), f, Color(30, 30, 35, 255));
            cx += m_columns[ci].width;
        }
    }
}

auto TreeView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "TreeView",
        .properties = {
            { .name = "selected_row", .type = "int", .default_value = "-1", .required = false, .note = "选中可见行(-1=无)", .json_type = "integer" },
        },
        .events = { "on_select", "on_toggle" },
        .children_policy = "none",
        .examples = { R"(au::TreeView({ {"root", {{"child"}}} }))" },
    };
}

auto TreeView::on_pointer_event(MouseEvent &e) -> void {
    if (e.action != MouseAction::Press) {
        Widget::on_pointer_event(e);
        return;
    }
    const int row = static_cast<int>(e.local_position.y / m_aurora_row_height);
    if (row >= 0 && std::cmp_less(row, visible_count())) {
        const float indent = static_cast<float>(visible_depth(row)) * m_aurora_indent;
        if (e.local_position.x >= indent && e.local_position.x < indent + m_aurora_arrow_zone) {
            toggle(row);
        } else {
            select(row);
        }
    }
    e.handled = true;
}

auto TreeView::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["selected_row"] = m_selected.get();
    props["visible_count"] = static_cast<int>(visible_count());
}

auto TreeView::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    const float w = c.max.is_finite() ? c.max.width : 240.0f;
    return c.constrain(Size{ .width = w, .height = static_cast<float>(visible_count()) * m_aurora_row_height });
}

auto TreeView::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0f;
    int row = 0;
    const int sel = m_selected.get();
    std::function<void(const TreeItem &, int)> draw = [&](const TreeItem &item, int depth) -> void {
        const float y = bounds.origin.y + (static_cast<float>(row) * m_aurora_row_height);
        if (row == sel) {
            p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = y },
                              .size = Size{ .width = bounds.size.width, .height = m_aurora_row_height } },
                        Color(0, 122, 255, 30));
        }
        const float indent = bounds.origin.x + (static_cast<float>(depth) * m_aurora_indent);
        if (!item.is_leaf()) {
            p.draw_text(Rect{ .origin = Point{ .x = indent + 4.0f, .y = y + 5.0f },
                              .size = Size{ .width = 14.0f, .height = m_aurora_row_height - 8.0f } },
                        item.expanded ? "v" : ">", f, Color(110, 110, 115, 255));
        }
        p.draw_text(Rect{ .origin = Point{ .x = indent + m_aurora_arrow_zone, .y = y + 5.0f },
                          .size = Size{ .width = bounds.size.width - indent - m_aurora_arrow_zone,
                                        .height = m_aurora_row_height - 8.0f } },
                    item.label, f, Color(30, 30, 35, 255));
        ++row;
        if (item.expanded) {
            for (const auto &ch : item.children) {
                draw(ch, depth + 1);
            }
        }
    };
    for (const auto &r : m_roots) {
        draw(r, 0);
    }
}

auto ListView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "ListView",
        .properties = {
            { .name = "items", .type = "vector<string>", .default_value = "[]", .required = true, .note = "行数据", .json_type = "array" },
            { .name = "multi_select", .type = "bool", .default_value = "false", .required = false, .note = "多选模式", .json_type = "boolean" },
        },
        .events = { "on_select", "on_remove" },
        .children_policy = "none",
        .examples = { R"(au::ListView({"Alpha", "Beta"}))" },
    };
}

auto ListView::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Press) {
        const int row = static_cast<int>(e.local_position.y / m_aurora_row_height);
        if (row >= 0 && std::cmp_less(row, m_items.size())) {
            select(row);
        }
        e.handled = true;
        return;
    }
    Widget::on_pointer_event(e);
}

auto ListView::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    Json items = Json::array();
    for (const auto &s : m_items) {
        items.push_back(s);
    }
    props["items"] = items;
    props["multi_select"] = m_multi;
}

auto ListView::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("items")) {
        if (props["items"].is_array()) {
            m_items.clear();
            for (const auto &s : props["items"]) {
                if (s.is_string()) {
                    m_items.push_back(s.get<std::string>());
                } else {
                    Diagnostics::degraded("items array elements must be strings", type_name(), "invalid-prop-value");
                }
            }
        } else {
            Diagnostics::degraded("items expects array", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("multi_select")) {
        if (props["multi_select"].is_boolean()) {
            m_multi = props["multi_select"].get<bool>();
        } else {
            Diagnostics::degraded("multi_select expects boolean", type_name(), "invalid-prop-value");
        }
    }
}

auto ListView::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    const float w = c.max.is_finite() ? c.max.width : 200.0f;
    return c.constrain(Size{ .width = w, .height = static_cast<float>(m_items.size()) * m_aurora_row_height });
}

auto ListView::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0f;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const float y = bounds.origin.y + (static_cast<float>(i) * m_aurora_row_height);
        if (is_selected(static_cast<int>(i))) {
            p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = y },
                              .size = Size{ .width = bounds.size.width, .height = m_aurora_row_height } },
                        Color(0, 122, 255, 30));
        }
        p.draw_text(Rect{ .origin = Point{ .x = bounds.origin.x + 10.0f, .y = y + 5.0f },
                          .size = Size{ .width = bounds.size.width - 16.0f, .height = m_aurora_row_height - 8.0f } },
                    m_items[i], f, Color(30, 30, 35, 255));
    }
}

} // namespace aurora
