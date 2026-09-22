/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/aria_protocol.h
/// 测试说明: Wasm/ARIA 平台桥的平台中立折算层 —— 角色映射全表（13 值逐一钉 WAI-ARIA 1.2
///           role 名）、动作名表与主点击动作优先级、属性折算（label 去重/checked/level/
///           range 数值格式/aria-hidden/tabindex/multiline 限定）、RFC 8259 最小集转义、
///           全量与 ops 载荷结构（remove→add→move→update 确定序 + 绝对 focus）、播报载荷、
///           aria-labelledby IDREF 投影与 aria-label 互斥

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/widget/a11y_diff.h"
#include "aurora/window/detail/aria_protocol.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_aria_protocol {

using aurora::a11y::NodeSnapshot;
using aurora::a11y::TreeSnapshot;
// TEST-R9：测试禁 using-directive，折算层符号逐名引入。
using aurora::detail::aria_actions_text;
using aurora::detail::aria_announce_json;
using aurora::detail::aria_element_of;
using aurora::detail::aria_full_json;
using aurora::detail::aria_json_escape;
using aurora::detail::aria_ops_json;
using aurora::detail::aria_primary_click_action;
using aurora::detail::aria_role_of;
using aurora::detail::aria_tree_of;
using aurora::detail::AriaAttr;
using aurora::detail::AriaElement;
using aurora::detail::focus_id_of;

namespace {

/// @brief 纯数据构造快照节点（widget 恒空：折算层不触碰活指针，纯快照即可全覆盖）。
[[nodiscard]] auto node(std::uint64_t id, std::uint64_t parent_id, AccessibilityRole role, std::string name)
    -> NodeSnapshot {
    NodeSnapshot n;
    n.id = id;
    n.parent_id = parent_id;
    n.node.id = id;
    n.node.role = role;
    n.node.name = std::move(name);
    n.node.actions = default_actions(role);
    n.node.state.visible = true;
    return n;
}

/// @brief 手搭快照：flat + by_id + children_of 三表同源（先序入参序即父子序）。
[[nodiscard]] auto snap(std::vector<NodeSnapshot> flat) -> TreeSnapshot {
    TreeSnapshot s;
    for (NodeSnapshot &n : flat) {
        s.by_id[n.id] = s.flat.size();
        s.children_of[n.parent_id].push_back(n.id);
        s.flat.push_back(std::move(n));
    }
    return s;
}

/// @brief 取属性值（未命中回空串）——表驱动断言的读面小工具。
[[nodiscard]] auto attr_of(const AriaElement &el, std::string_view key) -> std::string {
    for (const AriaAttr &a : el.attrs) {
        if (a.key == key) {
            return a.value;
        }
    }
    return {};
}

/// @brief 属性键序（折算层「确定序」的断言形态）。
[[nodiscard]] auto attr_keys(const AriaElement &el) -> std::string {
    std::string out;
    for (const AriaAttr &a : el.attrs) {
        if (!out.empty()) {
            out += ' ';
        }
        out += a.key;
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(aria_role_table_covers_all_13_roles) {
    // 逐名钉 ARIA 1.2 role 名——错一个字母读屏即降级为 generic。
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Generic) == "generic");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Button) == "button");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Text) == "text");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::TextInput) == "textbox");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Checkbox) == "checkbox");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Switch) == "switch");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Slider) == "slider");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Image) == "img");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::List) == "list");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::ListItem) == "listitem");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Header) == "heading");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Progress) == "progressbar");
    AURORA_TEST_CHECK(aria_role_of(AccessibilityRole::Dialog) == "dialog");
}

AURORA_TEST_CASE(aria_actions_text_deterministic_order) {
    using A = AccessibilityAction;
    AURORA_TEST_CHECK(aria_actions_text(A::None).empty());
    AURORA_TEST_CHECK(aria_actions_text(A::Focus) == "focus");
    // 序 = 表序（focus click invoke toggle select value scroll-*），与入参组合方式无关。
    AURORA_TEST_CHECK(aria_actions_text(A::Toggle | A::Focus | A::Click) == "focus click toggle");
    AURORA_TEST_CHECK(aria_actions_text(A::ScrollIntoView | A::ScrollUp | A::Value) ==
                      "value scroll-up scroll-into-view");
}

AURORA_TEST_CASE(primary_click_action_priority_chain) {
    AccessibilityNode n;
    AURORA_TEST_CHECK(aria_primary_click_action(n) == 0);  // 无动作 ⇒ JS 不监听
    n.actions = AccessibilityAction::Focus;
    AURORA_TEST_CHECK(aria_primary_click_action(n) == static_cast<std::uint16_t>(AccessibilityAction::Focus));
    // Invoke > Toggle > Click > Select > Value > Focus：全组合命中最高位 Invoke。
    using A = AccessibilityAction;
    n.actions = A::Focus | A::Value | A::Select | A::Click | A::Toggle | A::Invoke;
    AURORA_TEST_CHECK(aria_primary_click_action(n) == static_cast<std::uint16_t>(A::Invoke));
    n.actions = A::Focus | A::Click | A::Toggle;
    AURORA_TEST_CHECK(aria_primary_click_action(n) == static_cast<std::uint16_t>(A::Toggle));
    n.actions = A::Click | A::Value;
    AURORA_TEST_CHECK(aria_primary_click_action(n) == static_cast<std::uint16_t>(A::Click));
}

AURORA_TEST_CASE(element_button_attrs_exact_order) {
    const NodeSnapshot n = node(42, 7, AccessibilityRole::Button, "OK");
    const AriaElement el = aria_element_of(n);
    AURORA_TEST_CHECK(el.id == 42);
    AURORA_TEST_CHECK(el.parent_id == 7);
    AURORA_TEST_CHECK(el.dom_id == "aurora-a11y-42");  // IDREF 规则：前缀 + runtime_id
    AURORA_TEST_CHECK(el.role == "button");
    AURORA_TEST_CHECK(el.content.empty());  // 按钮正文恒空，名字在 aria-label
    AURORA_TEST_CHECK(attr_keys(el) == "aria-label data-aurora-id data-aurora-actions");
    AURORA_TEST_CHECK(attr_of(el, "aria-label") == "OK");
    AURORA_TEST_CHECK(attr_of(el, "data-aurora-id") == "42");
    AURORA_TEST_CHECK(attr_of(el, "data-aurora-actions") == "focus click invoke");
    AURORA_TEST_CHECK(el.click_action == static_cast<std::uint16_t>(AccessibilityAction::Invoke));
}

AURORA_TEST_CASE(text_content_suppresses_redundant_label) {
    // Text 以正文承载 value/name；正文 == name 时不再标 aria-label（防读屏双读）。
    NodeSnapshot n = node(3, 0, AccessibilityRole::Text, "你好");
    AriaElement el = aria_element_of(n);
    AURORA_TEST_CHECK(el.content == "你好");
    AURORA_TEST_CHECK(attr_of(el, "aria-label").empty());

    n.node.value = "world";  // value 优先于 name 做正文；name 仍在，label 保留
    el = aria_element_of(n);
    AURORA_TEST_CHECK(el.content == "world");
    AURORA_TEST_CHECK(attr_of(el, "aria-label") == "你好");

    // TextInput：name = label，value = 正文，二者各归其位。
    n = node(4, 0, AccessibilityRole::TextInput, "邮箱");
    n.node.value = "a@b.c";
    el = aria_element_of(n);
    AURORA_TEST_CHECK(el.role == "textbox");
    AURORA_TEST_CHECK(el.content == "a@b.c");
    AURORA_TEST_CHECK(attr_of(el, "aria-label") == "邮箱");
}

AURORA_TEST_CASE(labelled_by_ref_projects_idref_and_suppresses_label) {
    // 引用式标签关联（`set_labelled_by`）：投 `aria-labelledby` IDREF，且**不再**发 `aria-label`
    // ——ARIA 里 labelledby 压制 label，两处同发等于把选择权丢给读屏实现。
    NodeSnapshot n = node(11, 0, AccessibilityRole::Checkbox, "音量");
    n.node.labelled_by = "vol-label";
    n.node.labelled_by_id = 5;
    AriaElement el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-labelledby") == "aurora-a11y-5");
    AURORA_TEST_CHECK(attr_of(el, "aria-label").empty());
    AURORA_TEST_CHECK(attr_keys(el) == "aria-labelledby data-aurora-id data-aurora-actions");

    // 未解析出目标（id 为 0 ⇒ 树内未命中/环上/目标无名）⇒ 回落自身名的既有折算。
    n.node.labelled_by_id = 0;
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-labelledby").empty());
    AURORA_TEST_CHECK(attr_of(el, "aria-label") == "音量");
}

AURORA_TEST_CASE(state_attrs_map_exactly) {
    NodeSnapshot n = node(9, 0, AccessibilityRole::Checkbox, "同意");
    n.node.state.checkable = true;
    n.node.state.checked = true;
    AriaElement el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-checked") == "true");
    n.node.state.checked = false;
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-checked") == "false");  // 三态外的显式 false

    n.node.state.selected = true;
    n.node.state.disabled = true;
    n.node.state.read_only = true;
    n.node.state.focusable = true;
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-selected") == "true");
    AURORA_TEST_CHECK(attr_of(el, "aria-disabled") == "true");
    AURORA_TEST_CHECK(attr_of(el, "aria-readonly") == "true");
    AURORA_TEST_CHECK(attr_of(el, "tabindex") == "0");

    // aria-hidden：非控制节点（装饰图 / 显式非语义）进 DOM 不进读屏。
    n.node.is_control = false;
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-hidden") == "true");

    // multiline 只在 textbox 上成立（其他角色无此属性语义）。
    NodeSnapshot t = node(10, 0, AccessibilityRole::TextInput, "备注");
    t.node.state.multiline = true;
    AURORA_TEST_CHECK(attr_of(aria_element_of(t), "aria-multiline") == "true");
    NodeSnapshot b = node(11, 0, AccessibilityRole::Button, "备注");
    b.node.state.multiline = true;
    AURORA_TEST_CHECK(attr_of(aria_element_of(b), "aria-multiline").empty());

    // heading 才发 aria-level；hint → aria-description 通用。
    NodeSnapshot h = node(12, 0, AccessibilityRole::Header, "标题");
    h.node.level = 2;
    AURORA_TEST_CHECK(attr_of(aria_element_of(h), "aria-level") == "2");
    NodeSnapshot x = node(13, 0, AccessibilityRole::Generic, "g");
    x.node.level = 3;
    x.node.hint = "帮助";
    AriaElement xe = aria_element_of(x);
    AURORA_TEST_CHECK(attr_of(xe, "aria-level").empty());
    AURORA_TEST_CHECK(attr_of(xe, "aria-description") == "帮助");
}

AURORA_TEST_CASE(range_number_formatting) {
    NodeSnapshot n = node(20, 0, AccessibilityRole::Slider, "音量");
    n.node.range = AccessibilityRange{.min = 0.0, .max = 100.0, .step = 1.0, .value = 42.0};
    AriaElement el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-valuemin") == "0");  // 整数值不留小数尾巴
    AURORA_TEST_CHECK(attr_of(el, "aria-valuemax") == "100");
    AURORA_TEST_CHECK(attr_of(el, "aria-valuenow") == "42");
    n.node.range = AccessibilityRange{.min = 0.0, .max = 1.0, .step = 0.0, .value = 0.25};
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-valuenow") == "0.25");  // 定点去尾零
    n.node.range =
        AccessibilityRange{.min = 0.0, .max = 1.0, .step = 0.0, .value = std::numeric_limits<double>::quiet_NaN()};
    el = aria_element_of(n);
    AURORA_TEST_CHECK(attr_of(el, "aria-valuenow") == "0");  // 非有限回落 0（宁缺位不错位）
}

AURORA_TEST_CASE(json_escape_minimal_set) {
    AURORA_TEST_CHECK(aria_json_escape(R"(he said "hi" c:\dir)") == R"(he said \"hi\" c:\\dir)");
    AURORA_TEST_CHECK(aria_json_escape("a\nb\tc\rd") == R"(a\nb\tc\rd)");
    AURORA_TEST_CHECK(aria_json_escape(std::string{"\x01\x1f\x7f", 3}) == R"(\u0001\u001f\u007f)");  // 控制字符与 DEL
    AURORA_TEST_CHECK(aria_json_escape("中文 ✅") == "中文 ✅");  // UTF-8 原样透传（JSON 允许）
}

AURORA_TEST_CASE(full_json_shape_and_focus) {
    TreeSnapshot s = snap({
        node(1, 0, AccessibilityRole::Generic, ""),
        node(2, 1, AccessibilityRole::Button, "Go"),
    });
    s.flat[1].node.state.focused = true;
    const std::string json = aria_full_json(aria_tree_of(s), focus_id_of(s), true);
    AURORA_TEST_CHECK(json.find(R"jx({"els":[{)jx") == 0);
    AURORA_TEST_CHECK(json.find(R"("id":1,"parent":0,"dom":"aurora-a11y-1","role":"generic")") != std::string::npos);
    // Generic 默认动作仅 Focus ⇒ click=1；Button ⇒ children 收尾、click=16（Invoke）。
    AURORA_TEST_CHECK(json.find(R"("children":[2],"click":1})") != std::string::npos);
    AURORA_TEST_CHECK(json.find(R"("children":[],"click":16})") != std::string::npos);
    AURORA_TEST_CHECK(json.find(R"("focus":2,"rtl":true})") != std::string::npos);
    AURORA_TEST_CHECK(focus_id_of(snap({node(1, 0, AccessibilityRole::Generic, "")})) == 0);
}

AURORA_TEST_CASE(ops_json_remove_add_update_shape) {
    // 旧树 1→[2,3]，新树 1→[3(label变),4]：2 删、4 插到 index1、3 换 label 且失位？
    // LCS([2,3],[3,4]) = [3] ⇒ 3 在保序内不算 move —— DOM 现实恰如此：删 2 后 append 4
    // 即收敛，无需 move。本例钉「增删改三op齐、move 缺席」。
    TreeSnapshot old_s = snap({
        node(1, 0, AccessibilityRole::Generic, ""),
        node(2, 1, AccessibilityRole::Button, "Go"),
        node(3, 1, AccessibilityRole::Text, "hi"),
    });
    TreeSnapshot new_s = snap({
        node(1, 0, AccessibilityRole::Generic, ""),
        node(3, 1, AccessibilityRole::Text, "hello"),
        node(4, 1, AccessibilityRole::Button, "New"),
    });
    new_s.flat[1].node.state.focused = true;
    const a11y::TreeDiff diff = a11y::diff_snapshots(old_s, new_s);
    const std::string ops = aria_ops_json(old_s, new_s, diff);
    AURORA_TEST_CHECK(ops.find(R"({"op":"remove","id":2})") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"add","index":1,"el":{"id":4,)jx") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"update","el":{"id":3,)jx") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"("op":"move")") == std::string::npos);
    // add 载荷整面携带新元素属性（label 直嵌，JS 无须二次折算）。
    AURORA_TEST_CHECK(ops.find(R"("aria-label","New")") != std::string::npos);
    // focus 绝对值收尾；update 不为 add 者重发（id 4 无 old 影）。
    AURORA_TEST_CHECK(ops.find(R"("focus":3})") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"update","el":{"id":4,)jx") == std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"update","el":{"id":2,)jx") == std::string::npos);  // 已删者不改面
}

AURORA_TEST_CASE(ops_json_move_update_can_coexist) {
    // 纯换序 + 同步换内容：move 只换位、update 只换面，两 op 并列、按确定序出。
    TreeSnapshot old_s = snap({
        node(1, 0, AccessibilityRole::Generic, ""),
        node(2, 1, AccessibilityRole::Button, "A"),
        node(3, 1, AccessibilityRole::Button, "B"),
    });
    TreeSnapshot new_s = snap({
        node(1, 0, AccessibilityRole::Generic, ""),
        node(3, 1, AccessibilityRole::Button, "B!"),
        node(2, 1, AccessibilityRole::Button, "A!"),
    });
    const a11y::TreeDiff diff = a11y::diff_snapshots(old_s, new_s);
    const std::string ops = aria_ops_json(old_s, new_s, diff);
    // LCS([2,3],[3,2]) 保序取 [2] ⇒ 换序责任落在 3：move 到新父第 0 位。
    AURORA_TEST_CHECK(ops.find(R"({"op":"move","id":3,"parent":1,"index":0})") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"update","el":{"id":3,)jx") != std::string::npos);
    AURORA_TEST_CHECK(ops.find(R"jx({"op":"update","el":{"id":2,)jx") != std::string::npos);
    // 新快照先序 [3,2] ⇒ update 按先序出（3 在 2 前）；move 段先于 update 段。
    const auto pos_move = ops.find(R"("op":"move")");
    const auto pos_u3 = ops.find(R"jx({"op":"update","el":{"id":3,)jx");
    const auto pos_u2 = ops.find(R"jx({"op":"update","el":{"id":2,)jx");
    AURORA_TEST_CHECK_TRUE(pos_move < pos_u3 && pos_u3 < pos_u2);
    AURORA_TEST_CHECK(ops.find(R"("focus":0})") != std::string::npos);  // 无焦点 ⇒ 绝对值 0 清位
}

AURORA_TEST_CASE(announce_json_payload) {
    AURORA_TEST_CHECK(aria_announce_json(R"(it's "fine")", 0) == R"({"text":"it's \"fine\"","target":0})");
    AURORA_TEST_CHECK(aria_announce_json("已保存", 77) == R"({"text":"已保存","target":77})");
}

}  // namespace aurora::test_cases::utest_aria_protocol
