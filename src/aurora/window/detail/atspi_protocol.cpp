// AT-SPI2 平台中立折算层实现 —— 见 `atspi_protocol.h` 头部注释的协议事实来源。
//
// 本文件**不得**引入 D-Bus / 平台头：所有对外应答都是 (快照 + Env + 入参) 的纯函数，
// 无头单测（`tests/unit/utest_atspi_protocol.cpp`）因此在任何平台、任何工具链下都跑真值。

#include "aurora/window/detail/atspi_protocol.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "aurora/core/a11y_text.h"
#include "aurora/core/accessibility.h"

namespace aurora::detail {

namespace {

/// @brief DIP 矩形 → 整数物理 px 的统一取整口径（原点向下取整、终点向上取整，保覆盖不丢列）。
auto round_rect(const Rect &r) -> AtspiRectI {
    const auto l = static_cast<std::int32_t>(std::floor(r.origin.x));
    const auto t = static_cast<std::int32_t>(std::floor(r.origin.y));
    const auto rr = static_cast<std::int32_t>(std::ceil(r.origin.x + r.size.width));
    const auto bb = static_cast<std::int32_t>(std::ceil(r.origin.y + r.size.height));
    return AtspiRectI{.x = l, .y = t, .width = rr - l, .height = bb - t};
}

/// @brief 快照/合成节点统一入口：把 DIP 盒按坐标系折算成物理 px 矩形。
auto box_px(const AtspiEnv &env, const Rect &dip, std::uint32_t coord) -> AtspiRectI {
    // Env 的换算是「Rect→RectI」两段式：先折屏幕，再按坐标系平移。
    AtspiRectI screen = env.to_screen_px ? env.to_screen_px(dip) : round_rect(dip);
    if (coord == atspi::coord_window) {
        if (env.to_window_px) {
            return env.to_window_px(dip);
        }
        return AtspiRectI{.x = screen.x - env.window_origin_x,
                          .y = screen.y - env.window_origin_y,
                          .width = screen.width,
                          .height = screen.height};
    }
    if (coord == atspi::coord_parent) {
        // PARENT 系：以父的屏幕盒原点为基准；父原点未知时退化为 WINDOW 系（如实申报口径）。
        return AtspiRectI{.x = screen.x - env.window_origin_x,
                          .y = screen.y - env.window_origin_y,
                          .width = screen.width,
                          .height = screen.height};
    }
    return screen;
}

}  // namespace

// ============================================================================
// 公开纯函数
// ============================================================================

auto atspi_role_of(const AccessibilityNode &n) -> std::uint32_t {
    switch (n.role) {
        case AccessibilityRole::Generic:
            return atspi::role_panel;
        case AccessibilityRole::Button:
            return atspi::role_push_button;
        case AccessibilityRole::Text:
            return atspi::role_static;
        case AccessibilityRole::TextInput:
            return n.state.password ? atspi::role_password_text : atspi::role_entry;
        case AccessibilityRole::Checkbox:
            return atspi::role_check_box;
        case AccessibilityRole::Switch:
            return atspi::role_switch;
        case AccessibilityRole::Slider:
            return atspi::role_slider;
        case AccessibilityRole::Image:
            return atspi::role_image;
        case AccessibilityRole::List:
            return atspi::role_list;
        case AccessibilityRole::ListItem:
            return atspi::role_list_item;
        case AccessibilityRole::Header:
            return atspi::role_heading;
        case AccessibilityRole::Progress:
            return atspi::role_progress_bar;
        case AccessibilityRole::Dialog:
            return atspi::role_dialog;
        default:
            return atspi::role_unknown;
    }
}

auto atspi_role_name(std::uint32_t role) -> std::string {
    switch (role) {
        case atspi::role_application:
            return "application";
        case atspi::role_frame:
            return "frame";
        case atspi::role_panel:
            return "panel";
        case atspi::role_push_button:
            return "push button";
        case atspi::role_toggle_button:
            return "toggle button";
        case atspi::role_check_box:
            return "check box";
        case atspi::role_radio_button:
            return "radio button";
        case atspi::role_switch:
            return "switch";
        case atspi::role_slider:
            return "slider";
        case atspi::role_progress_bar:
            return "progress bar";
        case atspi::role_text:
            return "text";
        case atspi::role_entry:
            return "entry";
        case atspi::role_password_text:
            return "password text";
        case atspi::role_static:
            return "static";
        case atspi::role_label:
            return "label";
        case atspi::role_image:
            return "image";
        case atspi::role_list:
            return "list";
        case atspi::role_list_item:
            return "list item";
        case atspi::role_heading:
            return "heading";
        case atspi::role_dialog:
            return "dialog";
        case atspi::role_section:
            return "section";
        case atspi::role_notification:
            return "notification";
        // role_unknown 与其余未列角色一并映射为 "unknown"。
        default:
            return "unknown";
    }
}

// AtspiStateType → 规范状态名：`atspi-constants.h` 枚举**逐位序**（INVALID=0 … READ_ONLY=43），
// 名字 = 去前缀小写、下划线转连字符 —— 与 GLib 枚举 nick 及 libatspi `set_by_name` 同源，
// 是 `object:state-changed:<name>` 事件 minor 的单一来源（atk-adaptor 发送侧把 ATK 的
// pname 原样转发，两侧名字必须逐字一致，Orca 按类型串匹配）。
auto atspi_state_name(std::uint32_t state) -> const char * {
    constexpr const char *k_names[] = {
        "invalid",         "active",
        "armed",           "busy",
        "checked",         "collapsed",
        "defunct",         "editable",
        "enabled",         "expandable",
        "expanded",        "focusable",
        "focused",         "has-tooltip",
        "horizontal",      "iconified",
        "modal",           "multi-line",
        "multiselectable", "opaque",
        "pressed",         "resizable",
        "selectable",      "selected",
        "sensitive",       "showing",
        "single-line",     "stale",
        "transient",       "vertical",
        "visible",         "manages-descendants",
        "indeterminate",   "required",
        "truncated",       "animated",
        "invalid-entry",   "supports-autocompletion",
        "selectable-text", "is-default",
        "visited",         "checkable",
        "has-popup",       "read-only",
    };
    constexpr std::size_t count = std::size(k_names);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): 下标已由上行的 count 上界判定约束
    return state < count ? k_names[state] : nullptr;
}

auto atspi_interfaces_of(const AccessibilityNode &n) -> std::vector<std::string> {
    std::vector<std::string> out{atspi::k_iface_accessible, atspi::k_iface_component};
    if (n.role == AccessibilityRole::Text || n.role == AccessibilityRole::TextInput) {
        out.emplace_back(atspi::k_iface_text);
    }
    if (n.range.has_value()) {
        out.emplace_back(atspi::k_iface_value);
    }
    if (!atspi_actions_of(n).empty()) {
        out.emplace_back(atspi::k_iface_action);
    }
    return out;
}

auto atspi_states_of(const AccessibilityNode &n, const std::vector<std::string> &ifaces) -> std::vector<std::uint32_t> {
    const auto has_iface = [&ifaces](const char *want) {
        return std::ranges::find(ifaces, std::string{want}) != ifaces.end();
    };
    std::vector<std::uint32_t> s;
    const auto add = [&s](std::uint32_t v) {
        if (std::ranges::find(s, v) == s.end()) {
            s.push_back(v);
        }
    };
    // AT-SPI 没有「DISABLED」状态位：禁用 ⇒ 抽掉 ENABLED/SENSITIVE（atk 同款表达）。
    if (!n.state.disabled) {
        add(atspi::state_enabled);
        add(atspi::state_sensitive);
    }
    if (n.state.visible && !n.state.offscreen) {
        add(atspi::state_showing);
        add(atspi::state_visible);
    }
    if (n.state.focusable) {
        add(atspi::state_focusable);
    }
    if (n.state.focused) {
        add(atspi::state_focused);
    }
    if (n.state.checkable) {
        add(atspi::state_checkable);
    }
    if (n.state.checked) {
        add(atspi::state_checked);
    }
    if (n.state.selected) {
        add(atspi::state_selected);
    }
    if (n.state.expandable) {
        add(atspi::state_expandable);
    }
    if (n.state.expanded) {
        add(atspi::state_expanded);
    }
    if (has_iface(atspi::k_iface_text)) {
        add(atspi::state_selectable);  // 注意：此 selectable 即「可选中」，与 selected 独立
        add(atspi::state_selectable_text);
        add(n.state.read_only ? atspi::state_read_only : atspi::state_editable);
        add(n.state.multiline ? atspi::state_multi_line : atspi::state_single_line);
    }
    if (n.role == AccessibilityRole::ListItem || n.role == AccessibilityRole::List) {
        add(atspi::state_selectable);
    }
    std::ranges::sort(s);
    return s;
}

auto atspi_actions_of(const AccessibilityNode &n) -> std::vector<AtspiActionRow> {
    std::vector<AtspiActionRow> rows;
    const auto push = [&rows, &n](const char *name, AccessibilityAction fallback, AccessibilityAction prefer) {
        const AccessibilityAction use = n.has_action(prefer) ? prefer : fallback;
        rows.push_back(AtspiActionRow{.name = name, .localized = name, .action = use});
    };
    if (n.has_action(AccessibilityAction::Click) || n.has_action(AccessibilityAction::Invoke)) {
        push("press", AccessibilityAction::Invoke, AccessibilityAction::Click);
    }
    if (n.has_action(AccessibilityAction::Toggle)) {
        push("toggle", AccessibilityAction::Toggle, AccessibilityAction::Toggle);
    }
    if (n.has_action(AccessibilityAction::Select)) {
        push("select", AccessibilityAction::Select, AccessibilityAction::Select);
    }
    if (n.has_action(AccessibilityAction::ScrollUp)) {
        push("scroll up", AccessibilityAction::ScrollUp, AccessibilityAction::ScrollUp);
    }
    if (n.has_action(AccessibilityAction::ScrollDown)) {
        push("scroll down", AccessibilityAction::ScrollDown, AccessibilityAction::ScrollDown);
    }
    if (n.has_action(AccessibilityAction::ScrollLeft)) {
        push("scroll left", AccessibilityAction::ScrollLeft, AccessibilityAction::ScrollLeft);
    }
    if (n.has_action(AccessibilityAction::ScrollRight)) {
        push("scroll right", AccessibilityAction::ScrollRight, AccessibilityAction::ScrollRight);
    }
    if (n.has_action(AccessibilityAction::ScrollIntoView)) {
        push("scroll to visible", AccessibilityAction::ScrollIntoView, AccessibilityAction::ScrollIntoView);
    }
    return rows;
}

auto atspi_cp_count(std::string_view utf8) -> std::size_t {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto [cp, len] = a11y::detail::decode_cp(utf8, i);
        i += (len == 0) ? 1 : len;
        (void)cp;
        ++n;
    }
    return n;
}

auto atspi_cp_slice(std::string_view utf8, std::int64_t start, std::int64_t end) -> std::string {
    // 越界夹紧 + end<0 = 文末（AT-SPI 客户端普遍按 (0,-1) 读全文；非法值不报错）。
    std::size_t i = 0;
    std::size_t cp = 0;
    const auto target = [&](std::int64_t t) {
        if (t <= 0) {
            return std::size_t{0};
        }
        return static_cast<std::size_t>(t);
    };
    const std::int64_t want_end = (end < 0) ? std::numeric_limits<std::int64_t>::max() : end;
    std::int64_t s_pos = -1;
    std::int64_t e_pos = -1;
    while (i <= utf8.size()) {
        if (static_cast<std::int64_t>(cp) == static_cast<std::int64_t>(target(start))) {
            s_pos = static_cast<std::int64_t>(i);
        }
        if (std::cmp_equal(cp, want_end)) {
            e_pos = static_cast<std::int64_t>(i);
        }
        if (s_pos >= 0 && e_pos >= 0) {
            break;
        }
        if (i >= utf8.size()) {
            break;
        }
        const auto [dec, len] = a11y::detail::decode_cp(utf8, i);
        (void)dec;
        i += (len == 0) ? 1 : len;
        ++cp;
    }
    if (s_pos < 0) {
        s_pos = static_cast<std::int64_t>(utf8.size());  // 起点越界 ⇒ 空串
    }
    if (e_pos < 0) {
        e_pos = static_cast<std::int64_t>(utf8.size());  // 终点越界/为 max ⇒ 到文末
    }
    if (e_pos <= s_pos) {
        return {};
    }
    return std::string(utf8.substr(static_cast<std::size_t>(s_pos), static_cast<std::size_t>(e_pos - s_pos)));
}

// ============================================================================
// AtspiModel
// ============================================================================

auto AtspiModel::sync(const a11y::TreeSnapshot &snap) -> void {
    snap_ = &snap;
    order_.clear();
    by_id_.clear();
    kids_.clear();
    std::unordered_map<std::uint64_t, std::string> old_paths;
    old_paths.swap(by_path_id_cache_);
    // 注：old_paths 用成员缓存中转（见 header 的 by_path_id_cache_）——保留「同 id ⇒ 同路径」
    // 的跨帧不变式；消失 id 的路径直接丢弃（编号单调，不复用）。
    order_.push_back(LiveNode{.id = k_atspi_app_id, .parent_id = 0, .path = env_.base_path});
    order_.push_back(LiveNode{.id = k_atspi_frame_id, .parent_id = k_atspi_app_id, .path = env_.base_path + "/frame"});
    kids_[k_atspi_app_id].push_back(k_atspi_frame_id);

    // 先序重建存活表：裁剪节点（!is_control && !is_content）不入表，其子挂到最近存活祖先。
    std::unordered_map<std::uint64_t, std::uint64_t> eff_parent;  // 原父 id → 有效父 id
    eff_parent[0] = k_atspi_frame_id;  // 快照根挂 Frame 下
    for (const a11y::NodeSnapshot &ns : snap.flat) {
        const bool keep = ns.node.is_control || ns.node.is_content;
        const auto it = eff_parent.find(ns.parent_id);
        const std::uint64_t parent = (it != eff_parent.end()) ? it->second : k_atspi_frame_id;
        if (!keep) {
            eff_parent[ns.id] = parent;  // 后续子节点上挂
            continue;
        }
        std::string path;
        if (const auto old = old_paths.find(ns.id); old != old_paths.end()) {
            path = old->second;
        } else {
            path = env_.base_path + "/" + std::to_string(++path_counter_);
        }
        order_.push_back(LiveNode{.id = ns.id, .parent_id = parent, .path = std::move(path)});
        kids_[parent].push_back(ns.id);
        eff_parent[ns.id] = ns.id;
    }
    by_id_.clear();
    by_path_.clear();
    for (std::size_t i = 0; i < order_.size(); ++i) {
        by_id_[order_[i].id] = i;
        by_path_[order_[i].path] = order_[i].id;
        by_path_id_cache_[order_[i].id] = order_[i].path;
    }
}

auto AtspiModel::id_of_path(const std::string &path) const -> std::optional<std::uint64_t> {
    const auto it = by_path_.find(path);
    if (it == by_path_.end()) {
        return std::nullopt;
    }
    return it->second;
}

auto AtspiModel::path_of_id(std::uint64_t id) const -> std::string {
    const auto *l = live(id);
    return (l == nullptr) ? std::string{atspi::k_null_path} : l->path;
}

auto AtspiModel::live(std::uint64_t id) const -> const LiveNode * {
    const auto it = by_id_.find(id);
    return (it == by_id_.end()) ? nullptr : &order_[it->second];
}

auto AtspiModel::exists(std::uint64_t id) const -> bool { return live(id) != nullptr; }

auto AtspiModel::node(std::uint64_t id) const -> const a11y::NodeSnapshot * {
    if (snap_ == nullptr || id == k_atspi_app_id || id == k_atspi_frame_id) {
        return nullptr;
    }
    return snap_->find(id);
}

auto AtspiModel::widget_of(std::uint64_t id) const -> Widget * {
    const a11y::NodeSnapshot *n = node(id);
    return (n != nullptr) ? const_cast<Widget *>(n->widget) : nullptr;  // NOLINT
}

auto AtspiModel::name(std::uint64_t id) const -> std::string {
    if (id == k_atspi_app_id) {
        return env_.app_name;
    }
    if (id == k_atspi_frame_id) {
        return env_.window_title;
    }
    const a11y::NodeSnapshot *n = node(id);
    return (n == nullptr) ? std::string{} : n->node.name;
}

auto AtspiModel::description(std::uint64_t id) const -> std::string {
    const a11y::NodeSnapshot *n = node(id);
    return (n == nullptr) ? std::string{} : n->node.hint;  // hint = 「更详细的描述」（Cache 行同列语义）
}

auto AtspiModel::help_text(std::uint64_t id) const -> std::string { return description(id); }

auto AtspiModel::accessible_id(std::uint64_t id) const -> std::string {
    if (node(id) == nullptr) {
        return {};
    }
    return std::to_string(id);  // runtime_id 稳定；AT 侧可用于 AutomationId 类比
}

auto AtspiModel::role(std::uint64_t id) const -> std::uint32_t {
    if (id == k_atspi_app_id) {
        return atspi::role_application;
    }
    if (id == k_atspi_frame_id) {
        return atspi::role_frame;
    }
    const a11y::NodeSnapshot *n = node(id);
    // 已销毁 id：角色无从谈起（DEAD 对象由 `states()` 回 DEFUNCT 表达，角色回落 unknown）。
    return (n == nullptr) ? atspi::role_unknown : atspi_role_of(n->node);
}

auto AtspiModel::role_name(std::uint64_t id) const -> std::string { return atspi_role_name(role(id)); }

auto AtspiModel::states(std::uint64_t id) const -> std::vector<std::uint32_t> {
    if (id == k_atspi_app_id) {
        return {atspi::state_enabled, atspi::state_sensitive};
    }
    if (id == k_atspi_frame_id) {
        return {atspi::state_active, atspi::state_enabled, atspi::state_sensitive, atspi::state_showing,
                atspi::state_visible};
    }
    const a11y::NodeSnapshot *n = node(id);
    if (n == nullptr) {
        return {atspi::state_defunct};
    }
    return atspi_states_of(n->node, atspi_interfaces_of(n->node));
}

auto AtspiModel::interfaces(std::uint64_t id) const -> std::vector<std::string> {
    if (id == k_atspi_app_id) {
        return {atspi::k_iface_accessible, atspi::k_iface_application, atspi::k_iface_socket};
    }
    if (id == k_atspi_frame_id) {
        return {atspi::k_iface_accessible, atspi::k_iface_component};
    }
    const a11y::NodeSnapshot *n = node(id);
    if (n == nullptr) {
        return {atspi::k_iface_accessible};
    }
    return atspi_interfaces_of(n->node);
}

auto AtspiModel::child_count(std::uint64_t id) const -> std::int32_t {
    const auto it = kids_.find(id);
    return static_cast<std::int32_t>((it == kids_.end()) ? 0 : it->second.size());
}

auto AtspiModel::children(std::uint64_t id) const -> std::vector<std::uint64_t> {
    const auto it = kids_.find(id);
    return (it == kids_.end()) ? std::vector<std::uint64_t>{} : it->second;
}

auto AtspiModel::child_at(std::uint64_t id, std::int32_t index) const -> std::optional<std::uint64_t> {
    const auto kids = children(id);
    if (index < 0 || static_cast<std::size_t>(index) >= kids.size()) {
        return std::nullopt;
    }
    return kids[static_cast<std::size_t>(index)];
}

auto AtspiModel::parent(std::uint64_t id) const -> std::uint64_t {
    const LiveNode *l = live(id);
    return (l == nullptr) ? 0 : l->parent_id;
}

auto AtspiModel::index_in_parent(std::uint64_t id) const -> std::int32_t {
    const LiveNode *l = live(id);
    if (l == nullptr || l->parent_id == 0) {
        return -1;
    }
    const auto kids = children(l->parent_id);
    const auto it = std::ranges::find(kids, id);
    return (it == kids.end()) ? -1 : static_cast<std::int32_t>(it - kids.begin());
}

auto AtspiModel::ref_of(std::uint64_t id) const -> AtspiRef {
    if (!exists(id)) {
        return AtspiRef::null();
    }
    return AtspiRef{.bus = env_.self_bus, .path = path_of_id(id)};
}

auto AtspiModel::application() const -> AtspiRef { return ref_of(k_atspi_app_id); }

auto AtspiModel::parent_ref(std::uint64_t id) const -> AtspiRef {
    const std::uint64_t p = parent(id);
    if (p == 0) {
        // App 的父：注册表根（桌面）——AT 据此把本应用挂回桌面树。
        return env_.registry_root;
    }
    return ref_of(p);
}

auto AtspiModel::text_of(std::uint64_t id) const -> std::string {
    if (const Widget *w = widget_of(id); w != nullptr) {
        if (auto t = w->accessibility_text(); !t.empty()) {
            return std::string{t};
        }
    }
    const a11y::NodeSnapshot *n = node(id);
    return (n == nullptr) ? std::string{} : n->node.value;
}

auto AtspiModel::has_text(std::uint64_t id) const -> bool {
    const a11y::NodeSnapshot *n = node(id);
    if (n == nullptr) {
        return false;
    }
    const auto ifaces = atspi_interfaces_of(n->node);
    return std::ranges::find(ifaces, std::string{atspi::k_iface_text}) != ifaces.end();
}

auto AtspiModel::text_char_count(std::uint64_t id) const -> std::int32_t {
    return static_cast<std::int32_t>(atspi_cp_count(text_of(id)));
}

auto AtspiModel::text_slice(std::uint64_t id, std::int32_t start, std::int32_t end) const -> std::string {
    return atspi_cp_slice(text_of(id), start, end);
}

auto AtspiModel::text_caret(std::uint64_t id) const -> std::int32_t {
    Widget *w = widget_of(id);
    if (w == nullptr) {
        return -1;
    }
    const auto sel = w->accessibility_selection();
    if (!sel.has_value()) {
        return -1;
    }
    // `sel->start` 是 UTF-8 字节偏移（`AccessibilityTextSelection` 契约）；AT-SPI 的
    // CaretOffset 按码点 ⇒ 数「字节前缀内的码点数」。
    const std::string full = text_of(id);
    const std::size_t byte = std::min(sel->start, full.size());
    return static_cast<std::int32_t>(atspi_cp_count(std::string_view{full}.substr(0, byte)));
}

auto AtspiModel::text_char_extents(std::uint64_t id, std::int32_t offset, std::uint32_t coord) const -> AtspiRectI {
    Widget *w = widget_of(id);
    if (w == nullptr || offset < 0) {
        return {};
    }
    const std::string full = text_of(id);
    // 码点偏移 → UTF-8 字节偏移（accessibility_char_bounds 的入参口径）。
    std::size_t byte = 0;
    std::int32_t cp = 0;
    while (byte < full.size() && cp < offset) {
        const auto [dec, len] = a11y::detail::decode_cp(full, byte);
        (void)dec;
        byte += (len == 0) ? 1 : len;
        ++cp;
    }
    const auto box = w->accessibility_char_bounds(byte);
    const Rect dip = box.value_or(w->paint_bounds());
    Rect local{
        .origin = Point{.x = dip.origin.x - w->paint_bounds().origin.x +
                             ((node(id) != nullptr) ? node(id)->node.bounds.origin.x : 0.0F),
                        .y = dip.origin.y - w->paint_bounds().origin.y +
                             ((node(id) != nullptr) ? node(id)->node.bounds.origin.y : 0.0F)},
        .size = dip.size,
    };
    return box_px(env_, local, coord);
}

auto AtspiModel::has_value(std::uint64_t id) const -> bool {
    const a11y::NodeSnapshot *n = node(id);
    return n != nullptr && n->node.range.has_value();
}

auto AtspiModel::value_min(std::uint64_t id) const -> double {
    const a11y::NodeSnapshot *n = node(id);
    return (n != nullptr && n->node.range) ? n->node.range->min : 0.0;
}

auto AtspiModel::value_max(std::uint64_t id) const -> double {
    const a11y::NodeSnapshot *n = node(id);
    return (n != nullptr && n->node.range) ? n->node.range->max : 0.0;
}

auto AtspiModel::value_increment(std::uint64_t id) const -> double {
    const a11y::NodeSnapshot *n = node(id);
    return (n != nullptr && n->node.range) ? n->node.range->step : 0.0;
}

auto AtspiModel::value_current(std::uint64_t id) const -> double {
    const a11y::NodeSnapshot *n = node(id);
    return (n != nullptr && n->node.range) ? n->node.range->value : 0.0;
}

auto AtspiModel::value_set(std::uint64_t id, double v) const -> bool {
    Widget *w = widget_of(id);
    if (w == nullptr || !env_.perform) {
        return false;
    }
    return env_.perform(w, AccessibilityActionRequest{.action = AccessibilityAction::Value, .number = v});
}

auto AtspiModel::actions(std::uint64_t id) const -> std::vector<AtspiActionRow> {
    const a11y::NodeSnapshot *n = node(id);
    if (n == nullptr) {
        return {};
    }
    return atspi_actions_of(n->node);
}

auto AtspiModel::do_action(std::uint64_t id, std::int32_t index) const -> bool {
    const auto rows = actions(id);
    if (index < 0 || static_cast<std::size_t>(index) >= rows.size() || !env_.perform) {
        return false;
    }
    Widget *w = widget_of(id);
    if (w == nullptr) {
        return false;
    }
    return env_.perform(w, AccessibilityActionRequest{.action = rows[static_cast<std::size_t>(index)].action});
}

auto AtspiModel::extents(std::uint64_t id, std::uint32_t coord) const -> AtspiRectI {
    if (id == k_atspi_frame_id) {
        const a11y::NodeSnapshot *root = ((snap_ != nullptr) && !snap_->flat.empty()) ? snap_->flat.data() : nullptr;
        if (root == nullptr) {
            return {};
        }
        return box_px(env_, root->node.bounds, coord);
    }
    const a11y::NodeSnapshot *n = node(id);
    if (n == nullptr) {
        return {};
    }
    return box_px(env_, n->node.bounds, coord);
}

auto AtspiModel::contains(std::uint64_t id, std::int32_t x, std::int32_t y, std::uint32_t coord) const -> bool {
    const AtspiRectI r = extents(id, coord);
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

auto AtspiModel::accessible_at_point(std::uint64_t id, std::int32_t x, std::int32_t y, std::uint32_t coord) const
    -> AtspiRef {
    // 入参一律折算到屏幕系再比对（PARENT 系按 WINDOW 同口径退化，申报见头注）。
    std::int32_t sx = x;
    std::int32_t sy = y;
    if (coord != atspi::coord_screen) {
        sx = x + env_.window_origin_x;
        sy = y + env_.window_origin_y;
    }
    if (!exists(id)) {
        return AtspiRef::null();
    }
    // 子树收集（含自身）：BFS 保证祖先恒在前 ⇒ 逆序即「后绘制/更深者在前」，首个命中即返回。
    std::vector<std::uint64_t> sub{id};
    for (std::size_t i = 0; i < sub.size(); ++i) {
        const auto kids = children(sub[i]);
        sub.insert(sub.end(), kids.begin(), kids.end());
    }
    for (unsigned long long lid : std::views::reverse(sub)) {
        if (lid == k_atspi_app_id) {
            continue;
        }
        const AtspiRectI r = extents(lid, atspi::coord_screen);
        if (r.width <= 0 || r.height <= 0) {
            continue;
        }
        if (sx >= r.x && sx < r.x + r.width && sy >= r.y && sy < r.y + r.height) {
            return ref_of(lid);
        }
    }
    return AtspiRef::null();
}

auto AtspiModel::prop_get(std::uint64_t id, const std::string &iface, const std::string &prop) const -> AtspiPropValue {
    using P = AtspiPropValue;
    const auto str = [](std::string s) {
        P v;
        v.kind = P::Kind::Str;
        v.str = std::move(s);
        return v;
    };
    const auto i32 = [](std::int32_t n) {
        P v;
        v.kind = P::Kind::I32;
        v.i32 = n;
        return v;
    };
    const auto u32 = [](std::uint32_t n) {
        P v;
        v.kind = P::Kind::U32;
        v.u32 = n;
        return v;
    };
    const auto dbl = [](double d) {
        P v;
        v.kind = P::Kind::Dbl;
        v.dbl = d;
        return v;
    };
    const auto ref = [&](std::uint64_t target) {
        P v;
        v.kind = P::Kind::Ref;
        v.ref = (target == 0) ? env_.registry_root : ref_of(target);
        return v;
    };
    if (iface == atspi::k_iface_application && id == k_atspi_app_id) {
        if (prop == "ToolkitName") {
            return str(env_.toolkit_name);
        }
        if (prop == "Version" || prop == "AtspiVersion") {
            return str("2.1");  // Application.xml：恒「2.1」（版本协商弃用）
        }
        if (prop == "ToolkitVersion") {
            return str(env_.toolkit_version);
        }
        if (prop == "InterfaceVersion") {
            return u32(2);
        }
        if (prop == "Id") {
            return i32(app_id_);
        }
        return {};
    }
    if (iface == atspi::k_iface_accessible) {
        if (prop == "version") {
            return u32(2);
        }
        if (prop == "Name") {
            return str(name(id));
        }
        if (prop == "Description") {
            return str(description(id));
        }
        if (prop == "Parent") {
            return ref(parent(id));
        }
        if (prop == "ChildCount") {
            return i32(child_count(id));
        }
        if (prop == "Locale") {
            return str("C.UTF-8");
        }
        if (prop == "AccessibleId") {
            return str(accessible_id(id));
        }
        if (prop == "HelpText") {
            return str(help_text(id));
        }
        return {};
    }
    if (iface == atspi::k_iface_text && has_text(id)) {
        if (prop == "version") {
            return u32(2);
        }
        if (prop == "CharacterCount") {
            return i32(text_char_count(id));
        }
        if (prop == "CaretOffset") {
            return i32(text_caret(id));
        }
        return {};
    }
    if (iface == atspi::k_iface_value && has_value(id)) {
        if (prop == "version") {
            return u32(2);
        }
        if (prop == "MinimumValue") {
            return dbl(value_min(id));
        }
        if (prop == "MaximumValue") {
            return dbl(value_max(id));
        }
        if (prop == "MinimumIncrement") {
            return dbl(value_increment(id));
        }
        if (prop == "CurrentValue") {
            return dbl(value_current(id));
        }
        if (prop == "Text") {
            return str((node(id) != nullptr) ? node(id)->node.value : std::string{});
        }
        return {};
    }
    if (iface == atspi::k_iface_action) {
        if (prop == "version") {
            return u32(2);
        }
        // libatspi 2.60 的 atspi_action_get_n_actions 走 Properties.Get("NActions") "i"
        // （`_atspi_dbus_get_property (obj, atspi_interface_action, "NActions", …)`），
        // 再按索引逐个调 GetName/GetLocalizedName/GetDescription/GetKeyBinding。
        if (prop == "NActions") {
            return i32(static_cast<std::int32_t>(actions(id).size()));
        }
        return {};
    }
    if (iface == atspi::k_iface_cache) {
        if (prop == "version") {
            return u32(2);
        }
        return {};
    }
    if (iface == atspi::k_iface_socket) {
        if (prop == "version") {
            return u32(2);
        }
        return {};
    }
    return {};
}

auto AtspiModel::prop_set(std::uint64_t id, const std::string &iface, const std::string &prop, const AtspiPropValue &v)
    -> bool {
    if (iface == atspi::k_iface_application && prop == "Id" && id == k_atspi_app_id &&
        v.kind == AtspiPropValue::Kind::I32) {
        app_id_ = v.i32;  // 注册表 Embed 握手回填
        return true;
    }
    if (iface == atspi::k_iface_value && prop == "CurrentValue" && has_value(id) &&
        v.kind == AtspiPropValue::Kind::Dbl) {
        return value_set(id, v.dbl);
    }
    return false;
}

auto AtspiModel::cache_rows() const -> std::vector<AtspiCacheRow> {
    std::vector<AtspiCacheRow> rows;
    rows.reserve(order_.size());
    const AtspiRef app = application();
    for (const LiveNode &l : order_) {
        AtspiCacheRow r;
        r.self = ref_of(l.id);
        r.app = app;
        r.parent = (l.parent_id == 0) ? env_.registry_root : ref_of(l.parent_id);
        r.index_in_parent = index_in_parent(l.id);
        r.child_count = child_count(l.id);
        r.interfaces = interfaces(l.id);
        r.name = name(l.id);
        r.role = role(l.id);
        r.description = description(l.id);
        r.states = states(l.id);
        if (l.id == k_atspi_app_id) {
            r.parent = AtspiRef::null();  // Cache.xml：application 角色 → null 引用
            r.index_in_parent = -1;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

auto AtspiModel::handles(const std::string &iface, const std::string &member) -> bool {
    static const std::unordered_map<std::string, std::unordered_set<std::string>> TABLE = {
        {atspi::k_iface_accessible,
         {"GetChildAtIndex", "GetChildren", "GetIndexInParent", "GetRelationSet", "GetRole", "GetRoleName",
          "GetLocalizedRoleName", "GetState", "GetAttributes", "GetApplication", "GetInterfaces"}},
        {atspi::k_iface_application, {"GetLocale", "GetApplicationBusAddress"}},
        {atspi::k_iface_component,
         {"Contains", "GetAccessibleAtPoint", "GetExtents", "GetPosition", "GetSize", "GetLayer", "GrabFocus"}},
        {atspi::k_iface_text,
         {"GetText", "GetCaretOffset", "SetCaretOffset", "GetCharacterExtents", "GetCharacterCount"}},
        {atspi::k_iface_value, {}},  // 纯 Properties 接口
        {atspi::k_iface_action,
         {"GetActions", "DoAction", "GetName", "GetLocalizedName", "GetDescription", "GetKeyBinding", "GetNActions"}},
        {atspi::k_iface_cache, {"GetItems"}},
        {atspi::k_iface_socket, {"Embed", "Embedded", "Unembed", "Available"}},
        {"org.freedesktop.DBus.Properties", {"Get", "GetAll", "Set"}},
        {"org.freedesktop.DBus.Introspectable", {"Introspect"}},
        {"org.freedesktop.DBus.Peer", {"Ping", "GetMachineId"}},
    };
    const auto it = TABLE.find(iface);
    if (it == TABLE.end()) {
        return false;
    }
    return it->second.contains(member);
}

}  // namespace aurora::detail
