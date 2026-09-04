#include "aurora/widget/data_widgets.h"

#include <utility>

#include "aurora/core/diagnostics.h"

namespace aurora {

auto DataTable::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "DataTable",
        .properties =
            {
                {.name = "columns",
                 .type = "vector<DataColumn>",
                 .default_value = "[]",
                 .required = true,
                 .note = "列描述",
                 .json_type = "array"},
                {.name = "row_count",
                 .type = "int",
                 .default_value = "0",
                 .required = false,
                 .note = "行数（只读）",
                 .json_type = "integer",
                 .enum_values = {},
                 .min_value = "0"},
                {.name = "selected_row",
                 .type = "int",
                 .default_value = "-1",
                 .required = false,
                 .note = "选中行(-1=无)",
                 .json_type = "integer"},
                {.name = "sort_column",
                 .type = "int",
                 .default_value = "-1",
                 .required = false,
                 .note = "排序列(-1=无)",
                 .json_type = "integer"},
            },
        .events = {"on_sort", "on_select"},
        .children_policy = "none",
        .invariants = {"row_count >= 0"},
        .examples = {R"(au::DataTable({{"Name", 120}, {"Age", 60}}, {{"Alice", "30"}}))"},
    };
}

auto DataTable::on_pointer_event(MouseEvent &e) -> void {
    if (e.action != MouseAction::Press) {
        Widget::on_pointer_event(e);
        return;
    }
    // 表头
    if (e.local_position.y < AURORA_HEADER_HEIGHT) {
        float x = 0.0F;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            if (e.local_position.x >= x && e.local_position.x < x + columns_[i].width) {
                sort_by(static_cast<int>(i));
                break;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            x += columns_[i].width;
        }
        e.is_handled = true;
        return;
    }
    // 数据行
    const int row = static_cast<int>((e.local_position.y - AURORA_HEADER_HEIGHT) / AURORA_ROW_HEIGHT);
    if (row >= 0 && std::cmp_less(row, rows_.size())) {
        select_row(row);
    }
    e.is_handled = true;
}

auto DataTable::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    Json cols = Json::array();
    for (const auto &c : columns_) {
        Json jc;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        jc["label"] = c.label;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        jc["width"] = c.width;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        jc["sortable"] = c.sortable;
        cols.push_back(jc);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["columns"] = cols;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["row_count"] = static_cast<int>(rows_.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["selected_row"] = selected_row_.get();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["sort_column"] = sort_column_;
}

auto DataTable::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    float w = 0.0F;
    for (const auto &col : columns_) {
        w += col.width;
    }
    const float h = AURORA_HEADER_HEIGHT + (static_cast<float>(rows_.size()) * AURORA_ROW_HEIGHT);
    return c.constrain(Size{.width = w, .height = h});
}

auto DataTable::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0F;
    // 表头
    const Rect head{.origin = bounds.origin,
                    .size = Size{.width = bounds.size.width, .height = AURORA_HEADER_HEIGHT}};
    p.fill_rect(head, Color(246, 246, 248, 255));
    float x = bounds.origin.x;
    for (const auto &col : columns_) {
        std::string label = col.label;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (sort_column_ >= 0 && &col == &columns_[static_cast<std::size_t>(sort_column_)]) {
            label += sort_order_ == SortOrder::Ascending ? " ^" : " v";
        }
        p.draw_text(Rect{.origin = Point{.x = x + 8.0F, .y = head.origin.y + 8.0F},
                         .size = Size{.width = col.width - 12.0F, .height = AURORA_HEADER_HEIGHT - 12.0F}},
                    label, f, Color(60, 60, 65, 255));
        x += col.width;
    }
    // 行
    const int sel = selected_row_.get();
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        const float ry = bounds.origin.y + AURORA_HEADER_HEIGHT + (static_cast<float>(r) * AURORA_ROW_HEIGHT);
        if (std::cmp_equal(r, sel)) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = ry},
                             .size = Size{.width = bounds.size.width, .height = AURORA_ROW_HEIGHT}},
                        Color(0, 122, 255, 30));
        } else if (r % 2 == 1) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = ry},
                             .size = Size{.width = bounds.size.width, .height = AURORA_ROW_HEIGHT}},
                        Color(249, 249, 251, 255));
        }
        float cx = bounds.origin.x;
        for (std::size_t ci = 0; ci < columns_.size(); ++ci) {
            p.draw_text(Rect{.origin = Point{.x = cx + 8.0F, .y = ry + 6.0F},
                             // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                             // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                             .size = Size{.width = columns_[ci].width - 12.0F, .height = AURORA_ROW_HEIGHT - 10.0F}},
                        cell(r, ci), f, Color(30, 30, 35, 255));
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            cx += columns_[ci].width;
        }
    }
}

auto TreeView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "TreeView",
        .properties =
            {
                {.name = "selected_row",
                 .type = "int",
                 .default_value = "-1",
                 .required = false,
                 .note = "选中可见行(-1=无)",
                 .json_type = "integer"},
            },
        .events = {"on_select", "on_toggle"},
        .children_policy = "none",
        .examples = {R"(au::TreeView({ {"root", {{"child"}}} }))"},
    };
}

auto TreeView::on_pointer_event(MouseEvent &e) -> void {
    if (e.action != MouseAction::Press) {
        Widget::on_pointer_event(e);
        return;
    }
    const int row = static_cast<int>(e.local_position.y / AURORA_ROW_HEIGHT);
    if (row >= 0 && std::cmp_less(row, visible_count())) {
        const float indent = static_cast<float>(visible_depth(row)) * AURORA_INDENT;
        if (e.local_position.x >= indent && e.local_position.x < indent + AURORA_ARROW_ZONE) {
            toggle(row);
        } else {
            select(row);
        }
    }
    e.is_handled = true;
}

auto TreeView::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["selected_row"] = selected_.get();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["visible_count"] = static_cast<int>(visible_count());
}

auto TreeView::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    const float w = c.max.is_finite() ? c.max.width : 240.0F;
    return c.constrain(Size{.width = w, .height = static_cast<float>(visible_count()) * AURORA_ROW_HEIGHT});
}

auto TreeView::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0F;
    int row = 0;
    const int sel = selected_.get();
    std::function<void(const TreeItem &, int)> draw = [&](const TreeItem &item, int depth) -> void {
        const float y = bounds.origin.y + (static_cast<float>(row) * AURORA_ROW_HEIGHT);
        if (row == sel) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = y},
                             .size = Size{.width = bounds.size.width, .height = AURORA_ROW_HEIGHT}},
                        Color(0, 122, 255, 30));
        }
        const float indent = bounds.origin.x + (static_cast<float>(depth) * AURORA_INDENT);
        if (!item.is_leaf()) {
            p.draw_text(Rect{.origin = Point{.x = indent + 4.0F, .y = y + 5.0F},
                             .size = Size{.width = 14.0F, .height = AURORA_ROW_HEIGHT - 8.0F}},
                        item.expanded ? "v" : ">", f, Color(110, 110, 115, 255));
        }
        p.draw_text(Rect{.origin = Point{.x = indent + AURORA_ARROW_ZONE, .y = y + 5.0F},
                         .size = Size{.width = bounds.size.width - indent - AURORA_ARROW_ZONE,
                                      .height = AURORA_ROW_HEIGHT - 8.0F}},
                    item.label, f, Color(30, 30, 35, 255));
        ++row;
        if (item.expanded) {
            for (const auto &ch : item.children) {
                draw(ch, depth + 1);
            }
        }
    };
    for (const auto &r : roots_) {
        draw(r, 0);
    }
}

auto ListView::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "ListView",
        .properties =
            {
                {.name = "items",
                 .type = "vector<string>",
                 .default_value = "[]",
                 .required = true,
                 .note = "行数据",
                 .json_type = "array"},
                {.name = "multi_select",
                 .type = "bool",
                 .default_value = "false",
                 .required = false,
                 .note = "多选模式",
                 .json_type = "boolean"},
            },
        .events = {"on_select", "on_remove"},
        .children_policy = "none",
        .examples = {R"(au::ListView({"Alpha", "Beta"}))"},
    };
}

auto ListView::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Press) {
        const int row = static_cast<int>(e.local_position.y / AURORA_ROW_HEIGHT);
        if (row >= 0 && std::cmp_less(row, items_.size())) {
            select(row);
        }
        e.is_handled = true;
        return;
    }
    Widget::on_pointer_event(e);
}

auto ListView::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    Json items = Json::array();
    for (const auto &s : items_) {
        items.push_back(s);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["items"] = items;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["multi_select"] = multi_;
}

auto ListView::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("items")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (props["items"].is_array()) {
            items_.clear();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            for (const auto &s : props["items"]) {
                if (s.is_string()) {
                    items_.push_back(s.get<std::string>());
                } else {
                    Diagnostics::degraded("items array elements must be strings", type_name(), "invalid-prop-value");
                }
            }
        } else {
            Diagnostics::degraded("items expects array", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("multi_select")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (props["multi_select"].is_boolean()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            multi_ = props["multi_select"].get<bool>();
        } else {
            Diagnostics::degraded("multi_select expects boolean", type_name(), "invalid-prop-value");
        }
    }
}

auto ListView::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    const float w = c.max.is_finite() ? c.max.width : 200.0F;
    return c.constrain(Size{.width = w, .height = static_cast<float>(items_.size()) * AURORA_ROW_HEIGHT});
}

auto ListView::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    Font f;
    f.size_pt = 12.0F;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const float y = bounds.origin.y + (static_cast<float>(i) * AURORA_ROW_HEIGHT);
        if (is_selected(static_cast<int>(i))) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = y},
                             .size = Size{.width = bounds.size.width, .height = AURORA_ROW_HEIGHT}},
                        Color(0, 122, 255, 30));
        }
        p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + 10.0F, .y = y + 5.0F},
                         .size = Size{.width = bounds.size.width - 16.0F, .height = AURORA_ROW_HEIGHT - 8.0F}},
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                    items_[i], f, Color(30, 30, 35, 255));
    }
}

}  // namespace aurora