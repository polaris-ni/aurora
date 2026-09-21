// Wasm/ARIA 平台桥的平台中立折算层实现 —— 规范事实来源见 `aria_protocol.h` 头部注释。
//
// 本文件**不得**引入 Emscripten / 平台头：所有产出都是 (快照 + diff + 入参) 的纯函数，
// 无头单测（`tests/unit/utest_aria_protocol.cpp`）因此在任何平台、任何工具链下都跑真值。

#include "aurora/window/detail/aria_protocol.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace aurora::detail {

namespace {

/// @brief 数值 → ARIA 数值属性字符串：整数值不留小数尾巴，非整数定点 6 位去尾零；
///        非有限值回落 "0"（ARIA 数值属性不接受 NaN/Infinity，宁缺位不错位）。
[[nodiscard]] auto format_number(double v) -> std::string {
    if (!std::isfinite(v)) {
        return "0";
    }
    if (v == std::floor(v) && std::fabs(v) < 1.0e15) {
        return std::to_string(static_cast<long long>(v));
    }
    std::string s = std::to_string(v);  // 定点 6 位小数（std::to_string 契约）
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1, std::string::npos);
        if (s.back() == '.') {
            s.pop_back();
        }
    }
    return s;
}

/// @brief 追加一条属性（各表拼装处统一走此口，保证「有值才写、写则成对」）。
auto push_attr(AriaElement &el, std::string_view key, const std::string &value) -> void {
    el.attrs.push_back(AriaAttr{.key = std::string{key}, .value = value});
}

/// @brief 快照内构造带 `child_ids` 的完整元素（add/update 载荷与全量共用）。
[[nodiscard]] auto make_element(const a11y::TreeSnapshot &snap, const a11y::NodeSnapshot &n)
    -> AriaElement {
    AriaElement el = aria_element_of(n);
    const auto it = snap.children_of.find(n.id);
    if (it != snap.children_of.end()) {
        el.child_ids = it->second;
    }
    return el;
}

auto append_els_json(std::string &out, const std::vector<AriaElement> &tree) -> void {
    out += R"({"els":[)";
    for (std::size_t i = 0; i < tree.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += aria_element_json(tree[i]);
    }
    out += "],";
}

}  // namespace

auto aria_role_of(const AccessibilityRole role) -> std::string_view {
    using enum AccessibilityRole;
    switch (role) {
        case Generic:
            return "generic";
        case Button:
            return "button";
        case Text:
            return "text";  // ARIA 1.2 `text`：Chromium 映射为 StaticText
        case TextInput:
            return "textbox";  // 配 aria-multiline / aria-readonly
        case Checkbox:
            return "checkbox";
        case Switch:
            return "switch";
        case Slider:
            return "slider";
        case Image:
            return "img";
        case List:
            return "list";
        case ListItem:
            return "listitem";
        case Header:
            return "heading";  // 配 aria-level
        case Progress:
            return "progressbar";
        case Dialog:
            return "dialog";
    }
    return "";  // 未来新角色：宁缺 role 不错映射
}

auto aria_actions_text(const AccessibilityAction actions) -> std::string {
    using A = AccessibilityAction;
    struct Row {
        A bit;
        std::string_view name;
    };
    // 确定序（表驱动单测逐字断言的依据）。
    static constexpr Row rows[] = {
        {A::Focus, "focus"},
        {A::Click, "click"},
        {A::Invoke, "invoke"},
        {A::Toggle, "toggle"},
        {A::Select, "select"},
        {A::Value, "value"},
        {A::ScrollUp, "scroll-up"},
        {A::ScrollDown, "scroll-down"},
        {A::ScrollLeft, "scroll-left"},
        {A::ScrollRight, "scroll-right"},
        {A::ScrollIntoView, "scroll-into-view"},
    };
    std::string out;
    for (const Row &r : rows) {
        if ((static_cast<std::uint16_t>(actions) & static_cast<std::uint16_t>(r.bit)) != 0) {
            if (!out.empty()) {
                out += ' ';
            }
            out += r.name;
        }
    }
    return out;
}

auto aria_primary_click_action(const AccessibilityNode &n) -> std::uint16_t {
    // 镜像元素一次点击只回灌一个动作（DOM click 无「哪个动作」的表达面），
    // 按读屏主路径优先：激活类 > 切换类 > 点击类 > 选择 > 取值 > 聚焦。
    using A = AccessibilityAction;
    static constexpr A order[] = {A::Invoke, A::Toggle, A::Click, A::Select, A::Value, A::Focus};
    for (const A a : order) {
        if (n.has_action(a)) {
            return static_cast<std::uint16_t>(a);
        }
    }
    return 0;
}

auto aria_element_of(const a11y::NodeSnapshot &n) -> AriaElement {
    const AccessibilityNode &node = n.node;
    AriaElement el;
    el.id = n.id;
    el.parent_id = n.parent_id;
    el.dom_id = "aurora-a11y-" + std::to_string(n.id);
    el.role = aria_role_of(node.role);
    el.click_action = aria_primary_click_action(node);

    // 内容规则：Text/TextInput 以元素正文承载 value（缺 value 回落 name，与 Name
    // 回退链同口径）；其余角色正文恒空（信息全在属性面）。
    const bool text_bearing =
        node.role == AccessibilityRole::Text || node.role == AccessibilityRole::TextInput;
    if (text_bearing) {
        el.content = node.value.empty() ? node.name : node.value;
    }

    // 属性表：固定次序（同一段注释即单测断言序）。
    if (!node.name.empty() && node.name != el.content) {
        push_attr(el, "aria-label", node.name);  // 正文已等于 name 时不再重复标注（防双读）
    }
    if (!node.hint.empty()) {
        push_attr(el, "aria-description", node.hint);
    }
    if (node.level.has_value() && node.role == AccessibilityRole::Header) {
        push_attr(el, "aria-level", std::to_string(*node.level));
    }
    if (node.state.checkable) {
        push_attr(el, "aria-checked", node.state.checked ? "true" : "false");
    }
    if (node.state.selected) {
        push_attr(el, "aria-selected", "true");
    }
    if (node.state.disabled) {
        push_attr(el, "aria-disabled", "true");
    }
    if (node.state.read_only) {
        push_attr(el, "aria-readonly", "true");
    }
    if (node.state.multiline && node.role == AccessibilityRole::TextInput) {
        push_attr(el, "aria-multiline", "true");
    }
    if (node.range.has_value()) {
        push_attr(el, "aria-valuemin", format_number(node.range->min));
        push_attr(el, "aria-valuemax", format_number(node.range->max));
        push_attr(el, "aria-valuenow", format_number(node.range->value));
    }
    if (!node.is_control) {
        push_attr(el, "aria-hidden", "true");  // 装饰图 / 显式非语义：进 DOM 不进读屏
    }
    if (node.state.focusable) {
        push_attr(el, "tabindex", "0");
    }
    push_attr(el, "data-aurora-id", std::to_string(n.id));
    if (node.actions != AccessibilityAction::None) {
        push_attr(el, "data-aurora-actions", aria_actions_text(node.actions));
    }
    return el;
}

auto aria_tree_of(const a11y::TreeSnapshot &snap) -> std::vector<AriaElement> {
    std::vector<AriaElement> out;
    out.reserve(snap.flat.size());
    for (const a11y::NodeSnapshot &n : snap.flat) {
        out.push_back(make_element(snap, n));
    }
    return out;
}

auto aria_json_escape(const std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += R"(\")"; break;
            case '\\': out += R"(\\)"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: {
                if (c < 0x20U || c == 0x7FU) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += R"(\u00)";
                    out += hex[(c >> 4U) & 0x0FU];
                    out += hex[c & 0x0FU];
                } else {
                    out += ch;  // UTF-8 多字节原样透传（JSON 允许）
                }
            }
        }
    }
    return out;
}

auto aria_element_json(const AriaElement &el) -> std::string {
    std::string out = "{";
    out += R"("id":)" + std::to_string(el.id);
    out += R"(,"parent":)" + std::to_string(el.parent_id);
    out += R"(,"dom":")" + aria_json_escape(el.dom_id) + '"';
    out += R"(,"role":")" + aria_json_escape(el.role) + '"';
    out += R"(,"attrs":[)";
    for (std::size_t i = 0; i < el.attrs.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += R"([")" + aria_json_escape(el.attrs[i].key) + R"(",")" +
               aria_json_escape(el.attrs[i].value) + "\"]";
    }
    out += R"(],"content":")" + aria_json_escape(el.content) + R"(","children":[)";
    for (std::size_t i = 0; i < el.child_ids.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += std::to_string(el.child_ids[i]);
    }
    out += R"(],"click":)" + std::to_string(el.click_action) + "}";
    return out;
}

auto aria_full_json(const std::vector<AriaElement> &tree, const std::uint64_t focus, const bool rtl)
    -> std::string {
    std::string out;
    append_els_json(out, tree);
    out += R"("focus":)" + std::to_string(focus);
    out += R"(,"rtl":)" + std::string(rtl ? "true" : "false");
    out += "}";
    return out;
}

auto aria_ops_json(const a11y::TreeSnapshot &old_, const a11y::TreeSnapshot &new_,
                   const a11y::TreeDiff &diff) -> std::string {
    std::string out = R"({"ops":[)";
    bool first = true;
    const auto sep = [&out, &first]() -> void {
        if (!first) {
            out += ',';
        }
        first = false;
    };

    // 1) remove：先腾空，后续 add/move 的 index 才以「余者」为基准。
    for (const std::uint64_t id : diff.removed) {
        sep();
        out += R"({"op":"remove","id":)" + std::to_string(id) + "}";
    }

    // 2) add：按新快照先序（父先于子），index = 在新父子序中的最终位。
    for (const a11y::NodeSnapshot &n : new_.flat) {
        if (old_.find(n.id) != nullptr) {
            continue;
        }
        sep();
        const AriaElement el = make_element(new_, n);
        std::size_t index = 0;
        if (const auto it = new_.children_of.find(n.parent_id); it != new_.children_of.end()) {
            for (const std::uint64_t sib : it->second) {
                if (sib == n.id) {
                    break;
                }
                ++index;
            }
        }
        out += R"({"op":"add","index":)" + std::to_string(index) + R"(,"el":)" +
               aria_element_json(el) + "}";
    }

    // 3) move：detached → 插入新父第 index 位（新快照口径的最终位，先删后插幂等）。
    for (const std::uint64_t id : diff.moved) {
        const a11y::NodeSnapshot *n = new_.find(id);
        if (n == nullptr) {
            continue;  // 已随 remove 离场，无需再复位
        }
        sep();
        std::size_t index = 0;
        if (const auto it = new_.children_of.find(n->parent_id); it != new_.children_of.end()) {
            for (const std::uint64_t sib : it->second) {
                if (sib == id) {
                    break;
                }
                ++index;
            }
        }
        out += R"({"op":"move","id":)" + std::to_string(id) + R"(,"parent":)" +
               std::to_string(n->parent_id) + R"(,"index":)" + std::to_string(index) + "}";
    }

    // 4) update：字段变化去重（add 者随 "add" 载荷整面首发，天然排除；move 者仍可能
    //    同时换内容，两 op 并列不冲突 —— JS 侧 move 只换位、update 只换面）。
    std::unordered_set<std::uint64_t> field_changed_ids;
    field_changed_ids.reserve(diff.updated.size());
    for (const auto &[uid, change] : diff.updated) {
        (void)change;  // 载荷整面重发，字段粒度仅决定「是否出现在 ops 里」。
        field_changed_ids.insert(uid);
    }
    for (const a11y::NodeSnapshot &n : new_.flat) {
        if (!field_changed_ids.contains(n.id) || old_.find(n.id) == nullptr) {
            continue;
        }
        sep();
        out += R"({"op":"update","el":)" + aria_element_json(make_element(new_, n)) + "}";
    }

    out += R"(],"focus":)" + std::to_string(focus_id_of(new_)) + "}";
    return out;
}

auto focus_id_of(const a11y::TreeSnapshot &snap) -> std::uint64_t {
    for (const a11y::NodeSnapshot &n : snap.flat) {
        if (n.node.state.focused) {
            return n.id;
        }
    }
    return 0;
}

auto aria_announce_json(const std::string &text, const std::uint64_t target_id) -> std::string {
    return R"({"text":")" + aria_json_escape(text) + R"(","target":)" + std::to_string(target_id) +
           "}";
}

}  // namespace aurora::detail
