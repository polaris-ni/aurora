// inspector_panel.cpp — InspectorPanel 实现（树形浏览器 + 属性编辑器）。
#include "aurora/widget/inspector_panel.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/inspect.h"

namespace aurora {

// 属性值 → 显示字符串（Inspector 属性面板用）
namespace {
auto aurora_infer_value_string(const Json &v) -> std::string {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<int>());
    }
    if (v.is_number_float()) {
        std::ostringstream oss;
        oss << v.get<float>();
        return oss.str();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return v.dump();
}

auto aurora_append_extra_props(std::vector<std::pair<std::string, std::string>> &rows, const Json &values,
                               const WidgetDescriptor &desc) -> void {
    if (!values.is_object()) {
        return;
    }
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string &key = it.key();
        bool found = false;
        for (const auto &pd : desc.properties) {
            if (pd.name == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            const std::string val_str = it->is_string() ? it->get<std::string>() : it->dump();
            rows.emplace_back(key, val_str);
        }
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 描述
// ---------------------------------------------------------------------------

InspectorPanel::InspectorPanel(std::function<Node()> root_getter, float tree_ratio)
    : root_getter_(std::move(root_getter)), ratio_(std::clamp(tree_ratio, 0.1F, 0.9F)) {
    if (root_getter_) {
        target_root_ = root_getter_();
    }
}

auto InspectorPanel::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "InspectorPanel",
        .properties =
            {
                {.name = "ratio",
                 .type = "float",
                 .default_value = "0.35",
                 .required = false,
                 .note = "左侧树占比(0~1)",
                 .json_type = "number",
                 .enum_values = {},
                 .min_value = "0",
                 .max_value = "1"},
            },
        .events = {"on_select_widget_"},
        .children_policy = "none",
        .invariants = {"ratio >= 0 && ratio <= 1"},
        .examples = {"au::InspectorPanel([&] { return my_widget_tree; })"},
    };
}

auto InspectorPanel::set_root(std::function<Node()> getter) -> void {
    root_getter_ = std::move(getter);
    if (root_getter_) {
        target_root_ = root_getter_();
    }
    needs_rebuild_ = true;
    mark_needs_layout();
    mark_needs_paint();
}

auto InspectorPanel::refresh() -> void {
    if (root_getter_) {
        target_root_ = root_getter_();
    }
    needs_rebuild_ = true;
    mark_needs_layout();
    mark_needs_paint();
}

auto InspectorPanel::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["ratio"] = ratio_;
}

auto InspectorPanel::export_code() const -> std::string {
    if (!target_root_) {
        return {};
    }
    const Json tree_json = dump_tree_json_full(target_root_);
    return serialization::to_code(tree_json);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

auto InspectorPanel::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    Size self = c.max;
    if (!c.max.is_finite()) {
        self = Size{.width = 400.0F, .height = 300.0F};
    }
    total_size_ = self;

    if (needs_rebuild_) {
        rebuild_tree();
        needs_rebuild_ = false;
    }

    // 布局子节点（如果有）
    for (Node &child : children_) {
        Constraints child_c;
        child_c.min = Size{.width = 0.0F, .height = 0.0F};
        child_c.max = self;
        child.widget().set_layout_parent(this);
        child.widget().layout(child_c, ctx);
    }

    return c.constrain(self);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

auto InspectorPanel::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    (void)ctx;
    const float total_w = bounds.size.width;
    const float total_h = bounds.size.height;
    const float tree_w = total_w * ratio_;
    const float props_x = tree_w + AURORA_HANDLE_SIZE;
    const float props_w = total_w - props_x;

    // 背景
    p.fill_rect(bounds, Color{250, 250, 252});

    // ---- 左侧：树形浏览器 ----
    const Rect tree_rect{.origin = bounds.origin, .size = Size{.width = tree_w, .height = total_h}};
    p.fill_rect(tree_rect, Color{245, 246, 250});

    // 树标题
    const Rect header_rect{.origin = bounds.origin, .size = Size{.width = tree_w, .height = AURORA_HEADER_HEIGHT}};
    p.fill_rect(header_rect, Color{235, 237, 242});
    p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + 8.0F, .y = bounds.origin.y + 6.0F},
                     .size = Size{.width = tree_w - 16.0F, .height = AURORA_HEADER_HEIGHT - 8.0F}},
                "Widget Tree", Font{.size_pt = 11.0F}, Color{80, 80, 100});

    // 树行
    const float tree_y_start = bounds.origin.y + AURORA_HEADER_HEIGHT;
    const std::size_t vc = visible_count();
    for (std::size_t i = 0; i < vc; ++i) {
        const float ry = tree_y_start + (static_cast<float>(i) * AURORA_ROW_HEIGHT);
        if (ry + AURORA_ROW_HEIGHT < bounds.origin.y) {
            continue;
        }
        if (ry > bounds.origin.y + total_h) {
            break;
        }

        const int depth = visible_depth(static_cast<int>(i));
        const std::string label =
            (visible_item(static_cast<int>(i)) != nullptr) ? visible_item(static_cast<int>(i))->label : std::string{};

        // 选中高亮
        const Widget *w = widget_for_row(static_cast<int>(i));
        if (w == selected_widget_) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = ry},
                             .size = Size{.width = tree_w, .height = AURORA_ROW_HEIGHT}},
                        Color{200, 220, 255});
        }

        // 缩进 + 标签
        const float indent = (static_cast<float>(depth) * 16.0F) + 8.0F;
        constexpr auto text_color = Color{40, 40, 50};
        p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + indent, .y = ry + 4.0F},
                         .size = Size{.width = tree_w - indent - 4.0F, .height = AURORA_ROW_HEIGHT - 6.0F}},
                    label, Font{.size_pt = 10.0F}, text_color);
    }

    // ---- 分隔条 ----
    const Rect handle_rect{.origin = Point{.x = bounds.origin.x + tree_w, .y = bounds.origin.y},
                           .size = Size{.width = AURORA_HANDLE_SIZE, .height = total_h}};
    p.fill_rect(handle_rect, dragging_ ? Color{0, 122, 255, 120} : Color{0, 0, 0, 24});

    // ---- 右侧：属性面板 ----
    const Rect props_rect{.origin = Point{.x = bounds.origin.x + props_x, .y = bounds.origin.y},
                          .size = Size{.width = props_w, .height = total_h}};
    p.fill_rect(props_rect, Color{255, 255, 255});

    // 属性标题
    const Rect props_header{.origin = Point{.x = bounds.origin.x + props_x, .y = bounds.origin.y},
                            .size = Size{.width = props_w, .height = AURORA_HEADER_HEIGHT}};
    p.fill_rect(props_header, Color{235, 237, 242});

    const std::string title_text = (selected_widget_ != nullptr) ? selected_widget_->type_name() : "(未选中)";
    p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + props_x + 8.0F, .y = bounds.origin.y + 6.0F},
                     .size = Size{.width = props_w - 16.0F, .height = AURORA_HEADER_HEIGHT - 8.0F}},
                title_text, Font{.size_pt = 11.0F}, Color{80, 80, 100});

    // Export Code 按钮（右侧 header 右端）
    {
        constexpr float btn_w = 80.0F;
        constexpr float btn_h = AURORA_HEADER_HEIGHT - 6.0F;
        const float btn_x = bounds.origin.x + props_x + props_w - btn_w - 4.0F;
        const float btn_y = bounds.origin.y + 3.0F;
        export_btn_rect_ =
            Rect{.origin = Point{.x = btn_x, .y = btn_y}, .size = Size{.width = btn_w, .height = btn_h}};
        p.fill_rect(export_btn_rect_, Color{0, 122, 255});
        p.draw_text(export_btn_rect_, "Export Code", Font{.size_pt = 9.0F}, Color{255, 255, 255});
    }

    // 属性行
    const float props_y_start = bounds.origin.y + AURORA_HEADER_HEIGHT;
    for (std::size_t i = 0; i < prop_rows_.size(); ++i) {
        const float ry = props_y_start + (static_cast<float>(i) * AURORA_ROW_HEIGHT);
        if (ry + AURORA_ROW_HEIGHT < bounds.origin.y) {
            continue;
        }
        if (ry > bounds.origin.y + total_h) {
            break;
        }

        // 交替行背景
        if (i % 2 == 1) {
            p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x + props_x, .y = ry},
                             .size = Size{.width = props_w, .height = AURORA_ROW_HEIGHT}},
                        Color{248, 248, 252});
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const auto &[key, val] = prop_rows_[i];
        // 属性名
        p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + props_x + AURORA_PROP_INDENT, .y = ry + 4.0F},
                         .size = Size{.width = props_w * 0.4F, .height = AURORA_ROW_HEIGHT - 6.0F}},
                    key, Font{.size_pt = 10.0F}, Color{100, 100, 120});
        // 属性值
        p.draw_text(Rect{.origin = Point{.x = bounds.origin.x + props_x + (props_w * 0.4F) + 4.0F, .y = ry + 4.0F},
                         .size = Size{.width = props_w * 0.55F, .height = AURORA_ROW_HEIGHT - 6.0F}},
                    val, Font{.size_pt = 10.0F}, Color{30, 30, 40});
    }
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------

auto InspectorPanel::on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * {
    (void)ctx;
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
auto InspectorPanel::on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
    -> std::vector<HitNode> {
    (void)ctx;
    // weak_from_this() 返回的 weak_ptr 已被安全拷贝进 HitNode（非悬垂）；
    // 此处抑制 GCC 对该标准库惯用法的已知 -Wdangling-pointer 误报。
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local)
               ? std::vector{HitNode{this, weak_from_this(), bounds.origin}}
               : std::vector<HitNode>{};
}
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// 挂载
// ---------------------------------------------------------------------------

auto InspectorPanel::on_mount(const BuildContext &ctx) -> void {
    for (Node &child : children_) {
        child.widget().mount(ctx);
    }
}

// ---------------------------------------------------------------------------
// 指针事件
// ---------------------------------------------------------------------------

auto InspectorPanel::on_pointer_event(MouseEvent &e) -> void {
    const float tree_w = total_size_.width * ratio_;

    switch (e.action) {
        case MouseAction::Press: {
            // 分隔条拖拽开始
            if (e.local_position.x >= tree_w && e.local_position.x <= tree_w + AURORA_HANDLE_SIZE) {
                dragging_ = true;
                e.is_handled = true;
                return;
            }
            // 左侧树区域点击 → 选中行
            if (e.local_position.x < tree_w) {
                handle_tree_click(static_cast<int>(e.local_position.y));
                e.is_handled = true;
                return;
            }
            // 右侧属性区域点击
            if (e.local_position.x > tree_w + AURORA_HANDLE_SIZE) {
                // 检查是否点击了 Export Code 按钮
                if (export_btn_rect_.contains(e.local_position)) {
                    const std::string code = export_code();
                    if (on_export_code) {
                        on_export_code(code);
                    }
                    AURORA_LOG_INFO("inspector", "Export Code clicked, generated ", code.size(), " chars");
                    e.is_handled = true;
                    return;
                }
                handle_props_click(static_cast<int>(e.local_position.y));
                e.is_handled = true;
                return;
            }
            break;
        }
        case MouseAction::Move:
            if (dragging_ && total_size_.width > 0.0F) {
                const float new_ratio = e.local_position.x / total_size_.width;
                ratio_ = std::clamp(new_ratio, 0.1F, 0.9F);
                mark_needs_layout();
                mark_needs_paint();
                e.is_handled = true;
                return;
            }
            break;
        case MouseAction::Release:
            if (dragging_) {
                dragging_ = false;
                e.is_handled = true;
                return;
            }
            break;
        default:
            break;
    }
    Widget::on_pointer_event(e);
}

// ---------------------------------------------------------------------------
// 树操作
// ---------------------------------------------------------------------------

auto InspectorPanel::rebuild_tree() -> void {
    tree_items_.clear();
    widget_map_.clear();

    if (!target_root_) {
        return;
    }

    tree_items_ = widget_tree_to_items(target_root_);

    // 按 tree_items 的可见序 + 原始树的 DFS 序构建映射
    std::function<void(const std::vector<TreeItem> &, const std::vector<Node> &)> build_map =
        [&](const std::vector<TreeItem> &items, const std::vector<Node> &nodes) -> void {
        for (std::size_t i = 0; i < items.size() && i < nodes.size(); ++i) {
            // child_nodes() const 返回 const Node，widget() 返回 const Widget &；
            // 内部映射需要非 const 指针以支持属性回写，使用 const_cast 安全转换。
            widget_map_.push_back(const_cast<Widget *>(&nodes[i].widget()));  // NOLINT
            if (items.at(i).expanded) {
                auto children = nodes.at(i).widget().child_nodes();
                build_map(items.at(i).children, children);
            }
        }
    };
    const auto root_children = target_root_.widget().child_nodes();
    // 根节点自身
    widget_map_.push_back(const_cast<Widget *>(&target_root_.widget()));  // NOLINT
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    build_map(tree_items_[0].children, root_children);
}

auto InspectorPanel::update_props_panel() -> void {
    prop_rows_.clear();
    if (selected_widget_ == nullptr) {
        return;
    }

    const WidgetDescriptor desc = selected_widget_->describe();
    Json values = Json::object();
    selected_widget_->serialize_props(values);

    // 按 describe 的属性顺序排列
    for (const auto &pd : desc.properties) {
        const std::string val_str =
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            values.contains(pd.name) ? aurora_infer_value_string(values[pd.name]) : pd.default_value;
        prop_rows_.emplace_back(pd.name, val_str);
    }

    // 补充 serialize_props 中不在 describe 里的属性
    aurora_append_extra_props(prop_rows_, values, desc);
}

auto InspectorPanel::handle_tree_click(int local_y) -> void {
    constexpr float tree_y_start = AURORA_HEADER_HEIGHT;
    if (static_cast<float>(local_y) < tree_y_start) {
        return;
    }

    const int row = static_cast<int>((static_cast<float>(local_y) - tree_y_start) / AURORA_ROW_HEIGHT);
    if (row < 0 || static_cast<std::size_t>(row) >= visible_count()) {
        return;
    }

    // 查找对应 Widget
    if (std::cmp_less(row, widget_map_.size())) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        selected_widget_ = widget_map_[static_cast<std::size_t>(row)];
        update_props_panel();
        mark_needs_paint();
        if (on_select_widget) {
            on_select_widget(selected_widget_);
        }
    }
}

auto InspectorPanel::handle_props_click(int local_y) -> void {
    (void)local_y;
    // MVP：属性面板点击暂不触发编辑（仅展示）
    // 后续可扩展为点击属性行弹出编辑控件
}

// ---------------------------------------------------------------------------
// TreeView 可见序辅助（与 TreeView 内部逻辑一致）
// ---------------------------------------------------------------------------

auto InspectorPanel::visible_count() const -> std::size_t {
    std::size_t n = 0;
    for (const auto &r : tree_items_) {
        count_visible(r, n);
    }
    return n;
}

auto InspectorPanel::visible_item(int row) const -> const TreeItem * {
    if (row < 0) {
        return nullptr;
    }
    int idx = row;
    for (const auto &r : tree_items_) {
        const TreeItem *found = find_visible(r, idx);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

auto InspectorPanel::visible_item_mut(int row) -> TreeItem * {
    if (row < 0) {
        return nullptr;
    }
    int idx = row;
    for (auto &r : tree_items_) {
        TreeItem *found = find_visible_mut(r, idx);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

auto InspectorPanel::count_visible(const TreeItem &item, std::size_t &n) -> void {
    ++n;
    if (item.expanded) {
        for (const auto &ch : item.children) {
            count_visible(ch, n);
        }
    }
}

auto InspectorPanel::find_visible(const TreeItem &item, int &idx) -> const TreeItem * {
    if (idx == 0) {
        return &item;
    }
    --idx;
    if (item.expanded) {
        for (const auto &ch : item.children) {
            const TreeItem *r = find_visible(ch, idx);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return nullptr;
}

auto InspectorPanel::find_visible_mut(TreeItem &item, int &idx) -> TreeItem * {
    if (idx == 0) {
        return &item;
    }
    --idx;
    if (item.expanded) {
        for (auto &ch : item.children) {
            TreeItem *r = find_visible_mut(ch, idx);
            if (r != nullptr) {
                return r;
            }
        }
    }
    return nullptr;
}

auto InspectorPanel::visible_depth(int row) const -> int {
    int idx = row;
    for (const auto &r : tree_items_) {
        const int d = depth_of(r, idx, 0);
        if (d >= 0) {
            return d;
        }
    }
    return 0;
}

auto InspectorPanel::depth_of(const TreeItem &item, int &idx, int depth) -> int {
    if (idx == 0) {
        idx = -1;
        return depth;
    }
    --idx;
    if (item.expanded) {
        for (const auto &ch : item.children) {
            const int d = depth_of(ch, idx, depth + 1);
            if (d >= 0) {
                return d;
            }
        }
    }
    return -1;
}

auto InspectorPanel::widget_for_row(int row) const -> Widget * {
    if (row < 0 || static_cast<std::size_t>(row) >= widget_map_.size()) {
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    return widget_map_[static_cast<std::size_t>(row)];
}

}  // namespace aurora