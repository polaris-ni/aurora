#pragma once

#include <cctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/widget/data_widgets.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 运行时可观测（规格 §3.4）：树转储 / 结构查询 / 状态探查。
 *
 * 全部复用 `Widget::child_nodes()` 与 `serialization::to_json`，不引入额外状态。
 * 单线程 UI 假设；均为 `inline`（头文件即可用，无需链接实现）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */

/// @brief 人类可读的缩进树（每行一个 widget 的 type_name）。
[[nodiscard]] inline auto dump_tree(const Node &root, int depth = 0) -> std::string {
    std::string out;
    for (int i = 0; i < depth; ++i) {
        out += "  ";
    }
    out += root.widget().type_name();
    out += '\n';
    for (const Node &child : root.widget().child_nodes()) {
        out += dump_tree(child, depth + 1);
    }
    return out;
}

/// @brief 生成单个节点的富文本标签（供 dump_tree_rich 使用）。
[[nodiscard]] inline auto node_label_for_dump(const Node &n) -> std::string {
    const Widget &w = n.widget();
    const std::string tn = w.type_name();
    const std::string id = n.id().empty() ? "" : ("#" + std::string(n.id()));
    const Rect b = n.bounds();
    const bool vis = w.show.get();
    std::string text;
    if (tn == "Text") {
        text = dynamic_cast<const Text &>(w).content.get().text;
    } else if (tn == "TextInput") {
        text = dynamic_cast<const TextInput &>(w).value();
    }
    Json j;
    w.serialize_props(j);
    std::string style;
    const auto pick = [&](const char *out_k, const char *ink) -> void {
        if (!j.contains(ink)) {
            return;
        }
        if (!style.empty()) {
            style += ", ";
        }
        style += std::string(out_k) + ": " + j[ink].dump();
    };
    pick("bg", "background_color");
    pick("bg", "color");
    pick("fg", "text_color");
    pick("radius", "corner_radius");
    pick("font_size", "font_size");
    pick("padding", "padding");
    std::string listeners;
    for (const auto &ev : w.describe().events) {
        if (!listeners.empty()) {
            listeners += ", ";
        }
        listeners += std::string(ev);
    }
    return tn + id + " { bounds: [" + std::to_string(static_cast<int>(b.origin.x)) + ", " +
           std::to_string(static_cast<int>(b.origin.y)) + ", " + std::to_string(static_cast<int>(b.size.width)) + ", " +
           std::to_string(static_cast<int>(b.size.height)) + "]; visible: " + (vis ? "true" : "false") + "; text: \"" +
           text + "\"; style: {" + style + "}; listeners: [" + listeners + "] }";
}

[[nodiscard]] inline auto tree_branch(bool tree_chars, bool is_last) -> std::string_view {
    if (!tree_chars) {
        return "";
    }
    return is_last ? "└─ " : "├─ ";
}

[[nodiscard]] inline auto tree_child_indent(bool tree_chars, bool is_last) -> std::string_view {
    if (!tree_chars) {
        return "  ";
    }
    return is_last ? "   " : "│  ";
}

/// @brief 富格式文本化 Widget 树（AI-First 规范 §6.4）：含 `#id`、bounds、visible、text、
/// style、listeners，并以 `├─ └─ │` 树形连接符呈现层级，供 AI 文本断言 / diff / 定位。
///
/// 输出示例：
/// @code
/// Column#root { bounds: [0, 0, 320, 240]; visible: true; text: ""; style: {}; listeners: [] }
/// ├─ Text { bounds: [8, 8, 40, 18]; visible: true; text: "Hello"; style: {}; listeners: [] }
/// └─ Button#ok { bounds: [8, 32, 64, 28]; visible: true; text: ""; style: {bg: ...}; listeners: [on_click] }
/// @endcode
///
/// @param root 根节点（建议在 `render_to_logical_snapshot` / `test::pump` 之后调用，bounds 方为非空）。
/// @param depth 缩进深度（内部递归用，外部调用传 0）。
/// @param tree_chars 是否使用 `├─ └─ │` 树形连接符（false 则使用纯空格缩进）。
[[nodiscard]] inline auto dump_tree_rich(const Node &root, int depth = 0, bool tree_chars = true) -> std::string {
    (void)depth;
    std::string out;

    std::function<void(const Node &, const std::string &, bool)> rec = [&](const Node &n, const std::string &prefix,
                                                                           bool is_last) -> void {
        out += prefix + std::string(tree_branch(tree_chars, is_last)) + node_label_for_dump(n) + "\n";
        const std::string child_prefix = prefix + std::string(tree_child_indent(tree_chars, is_last));
        const auto kids = n.widget().child_nodes();
        for (size_t i = 0; i < kids.size(); ++i) {
            rec(kids[i], child_prefix, i + 1 == kids.size());
        }
    };
    rec(root, "", true);
    return out;
}

/// @brief 结构化树（JSON）：每个节点含 `type` 与 `children`。
[[nodiscard]] inline auto dump_tree_json(const Node &root) -> Json {
    Json j = Json::object();
    j["type"] = root.widget().type_name();
    Json children = Json::array();
    for (const Node &child : root.widget().child_nodes()) {
        children.push_back(dump_tree_json(child));
    }
    j["children"] = children;
    return j;
}

/// @brief 按 type_name 精确匹配，返回子树中所有命中的节点（浅拷贝 shared_ptr）。
[[nodiscard]] inline auto query(std::string_view type, const Node &root) -> std::vector<Node> {
    std::vector<Node> out;
    std::function<void(const Node &)> walk = [&](const Node &n) -> void {
        if (n.widget().type_name() == type) {
            out.push_back(n);
        }
        for (const Node &child : n.widget().child_nodes()) {
            walk(child);
        }
    };
    walk(root);
    return out;
}

[[nodiscard]] inline auto is_digits_only(std::string_view s) -> bool {
    return std::ranges::all_of(s, [](char ch) -> bool { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
}

[[nodiscard]] inline auto advance_json_pointer(Json *&cur, std::string_view key) -> bool {
    if (is_digits_only(key)) {
        const std::size_t idx = std::stoul(std::string(key));
        if (!cur->is_array() || idx >= cur->size()) {
            return false;
        }
        cur = &(*cur)[idx];
    } else {
        if (!cur->is_object() || !cur->contains(std::string(key))) {
            return false;
        }
        cur = &(*cur)[std::string(key)];
    }
    return true;
}

/// @brief 沿 dump_tree_json 产出的 JSON 树，按 `/` 路径（支持对象键与数组下标）取状态片段。
/// 例：`get_state("children/0/type", root)` 返回根的第一个子节点的 type。未命中返回空 Json。
[[nodiscard]] inline auto get_state(std::string_view path, const Node &root) -> Json {
    Json tree = dump_tree_json(root);
    Json *cur = &tree;
    std::size_t i = 0;
    while (i < path.size()) {
        const std::size_t slash = path.find('/', i);
        const std::size_t len = (slash == std::string_view::npos) ? (path.size() - i) : (slash - i);
        const std::string key(path.substr(i, len));
        if (key.empty()) {
            if (slash == std::string_view::npos) {
                break;
            }
            i = slash + 1;
            continue;
        }
        if (!advance_json_pointer(cur, key)) {
            return Json{};
        }
        if (slash == std::string_view::npos) {
            break;
        }
        i = slash + 1;
    }
    return *cur; // 拷贝，避免悬垂引用
}

/// @brief 将 Widget 树递归转换为 TreeItem 树（供 TreeView 消费）。
/// 每个节点的 label 为 widget 的 type_name()，根节点默认展开。
[[nodiscard]] inline auto widget_tree_to_items(const Node &root) -> std::vector<TreeItem> {
    std::vector<TreeItem> items;
    std::function<void(const Node &, bool)> walk = [&](const Node &n, bool expand) -> void {
        TreeItem item;
        item.label = n.widget().type_name();
        item.expanded = expand;
        for (const Node &child : n.widget().child_nodes()) {
            walk(child, false);
            item.children.push_back(std::move(items.back()));
            items.pop_back();
        }
        items.push_back(std::move(item));
    };
    walk(root, true);
    return items;
}

/// @brief 含属性的完整 JSON 快照：每个节点含 { type, props, children }。
/// 扩展 dump_tree_json，增加 serialize_props 输出的属性对象。
[[nodiscard]] inline auto dump_tree_json_full(const Node &root) -> Json {
    Json j = Json::object();
    j["type"] = root.widget().type_name();
    Json props = Json::object();
    root.widget().serialize_props(props);
    j["props"] = props;
    Json children = Json::array();
    for (const Node &child : root.widget().child_nodes()) {
        children.push_back(dump_tree_json_full(child));
    }
    j["children"] = children;
    return j;
}

/// @brief 按树路径定位节点（路径为子节点索引序列，如 "0/2/1"）。
/// 根节点为空路径或 ""。每段为子节点在 child_nodes() 中的下标。
/// 路径无效返回空 Node（bool 转换返回 false）。
/// @note 返回的 Node 内部 shared_ptr<Widget> 指向同一 widget 实例，但 Node 本身是副本。
[[nodiscard]] inline auto find_node_by_path(const Node &root, std::string_view path) -> Node {
    if (path.empty()) {
        return root;
    }
    // 收集路径段
    std::vector<std::size_t> indices;
    std::size_t i = 0;
    while (i < path.size()) {
        const std::size_t slash = path.find('/', i);
        const std::size_t len = (slash == std::string_view::npos) ? (path.size() - i) : (slash - i);
        const std::string seg(path.substr(i, len));
        if (!seg.empty()) {
            try {
                indices.push_back(std::stoul(seg));
            } catch (...) {
                return Node{}; // 无效路径段返回空 Node
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        i = slash + 1;
    }
    // 沿索引路径下降，每层保存 children 副本以保持 Node 存活
    // 使用 pairs 保存 (children 副本, 当前选中索引)
    struct Layer {
        std::vector<Node> children;
        std::size_t idx;
    };
    std::vector<Layer> layers;
    layers.reserve(indices.size()); // 预分配避免重分配失效
    // 初始层：根的子节点
    {
        auto children = root.widget().child_nodes();
        if (indices[0] >= children.size()) {
            return Node{};
        }
        layers.push_back(Layer{ .children = std::move(children), .idx = indices[0] });
    }
    // 后续层
    for (std::size_t d = 1; d < indices.size(); ++d) {
        const auto prev_idx = layers.size() - 1;
        auto children = layers[prev_idx].children[layers[prev_idx].idx].widget().child_nodes();
        if (indices[d] >= children.size()) {
            return Node{};
        }
        layers.push_back(Layer{ .children = std::move(children), .idx = indices[d] });
    }
    return layers.back().children[layers.back().idx];
}

/// @brief 获取 Widget 的当前属性快照（describe 元数据 + serialize_props 合并）。
/// 返回 JSON 对象含 "descriptor"（WidgetDescriptor 摘要）与 "values"（当前属性值）。
[[nodiscard]] inline auto get_widget_props(const Widget &w) -> Json {
    Json result = Json::object();
    // 描述元数据
    const WidgetDescriptor desc = w.describe();
    Json descriptor = Json::object();
    descriptor["name"] = desc.name;
    Json prop_names = Json::array();
    for (const auto &pd : desc.properties) {
        prop_names.push_back(pd.name);
    }
    descriptor["property_names"] = prop_names;
    result["descriptor"] = descriptor;
    // 当前属性值
    Json values = Json::object();
    w.serialize_props(values);
    result["values"] = values;
    return result;
}

/// @brief 单属性回写：构造仅含目标键的 JSON 对象，经 deserialize_props 写回。
inline void set_widget_prop(Widget &w, std::string_view key, const Json &value) {
    Json props = Json::object();
    props[std::string(key)] = value;
    w.deserialize_props(props);
}

} // namespace aurora
