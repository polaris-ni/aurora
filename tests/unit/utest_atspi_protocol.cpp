/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/atspi_protocol.h
/// 测试说明: AT-SPI2 平台中立折算层 —— 角色/状态/接口/动作四张折算表（序号钉 upstream
///           at-spi-constants.h，含 PUSH_BUTTON=43 别名与 SWITCH=130 等易错点）、码点偏移
///           切片（GetText(0,-1) 全文 / 越界夹紧）、AtspiModel 寻址（合成 App/Frame、
///           裁剪上挂、跨帧路径稳定）、Properties Get/Set、Cache 行形态与 handles 成员表

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/a11y_types.h"
#include "aurora/core/accessibility.h"
#include "aurora/widget/a11y_diff.h"
#include "aurora/widget/a11y_tree.h"
#include "aurora/widget/widget.h"
#include "aurora/window/detail/atspi_protocol.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_atspi_protocol {

using aurora::a11y::NodeSnapshot;
using aurora::a11y::TreeSnapshot;
// TEST-R9：测试禁 using-directive，折算层符号逐名引入。
using aurora::detail::atspi_actions_of;
using aurora::detail::atspi_cp_count;
using aurora::detail::atspi_cp_slice;
using aurora::detail::atspi_interfaces_of;
using aurora::detail::atspi_role_name;
using aurora::detail::atspi_role_of;
using aurora::detail::atspi_state_name;
using aurora::detail::atspi_states_of;
using aurora::detail::AtspiCacheRow;
using aurora::detail::AtspiEnv;
using aurora::detail::AtspiModel;
using aurora::detail::AtspiPropValue;
using aurora::detail::AtspiRef;
using aurora::detail::AURORA_ATSPI_APP_ID;
using aurora::detail::AURORA_ATSPI_FRAME_ID;
using aurora::detail::atspi::coord_screen;
using aurora::detail::atspi::coord_window;
using aurora::detail::atspi::k_iface_accessible;
using aurora::detail::atspi::k_iface_action;
using aurora::detail::atspi::k_iface_application;
using aurora::detail::atspi::k_iface_cache;
using aurora::detail::atspi::k_iface_component;
using aurora::detail::atspi::k_iface_socket;
using aurora::detail::atspi::k_iface_text;
using aurora::detail::atspi::k_iface_value;
using aurora::detail::atspi::k_null_path;
using aurora::detail::atspi::k_registry_root_path;
using aurora::detail::atspi::state_defunct;
using aurora::detail::atspi::state_editable;
using aurora::detail::atspi::state_enabled;
using aurora::detail::atspi::state_focusable;
using aurora::detail::atspi::state_multi_line;
using aurora::detail::atspi::state_read_only;
using aurora::detail::atspi::state_selectable_text;
using aurora::detail::atspi::state_sensitive;
using aurora::detail::atspi::state_showing;
using aurora::detail::atspi::state_single_line;
using aurora::detail::atspi::state_visible;

namespace {

/// @brief 可注入文本 / 选区的探针控件（Text 接口与动作执行路径需要活 widget）。
class ProbeWidget final : public LeafWidget {
  public:
    explicit ProbeWidget(std::string text, std::string type = "Probe")
        : text_(std::move(text)), type_(std::move(type)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return type_.c_str(); }
    [[nodiscard]] auto accessibility_text() const -> std::string_view override { return text_; }
    [[nodiscard]] auto accessibility_selection() const -> std::optional<AccessibilityTextSelection> override {
        return selection;
    }

    std::optional<AccessibilityTextSelection> selection;  ///< 测试直写（字节偏移口径）

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override { return {}; }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    std::string text_;
    std::string type_;
};

/// @brief 纯数据构造存活节点（widget 可选；缺省即「控件已不在」的纯快照形态）。
[[nodiscard]] auto leaf(std::uint64_t id, std::uint64_t parent_id, AccessibilityRole role, std::string name,
                        const Widget *widget = nullptr) -> NodeSnapshot {
    NodeSnapshot n;
    n.id = id;
    n.parent_id = parent_id;
    n.widget = widget;
    n.node.id = id;
    n.node.role = role;
    n.node.name = std::move(name);
    n.node.actions = default_actions(role);
    n.node.state.visible = true;
    return n;
}

[[nodiscard]] auto box(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 把节点装入快照（重建 by_id / children_of 索引；先序）。
[[nodiscard]] auto snapshot(std::vector<NodeSnapshot> flat) -> TreeSnapshot {
    TreeSnapshot snap;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        snap.by_id[flat[i].id] = i;
        snap.children_of[flat[i].parent_id].push_back(flat[i].id);
    }
    snap.flat = std::move(flat);
    return snap;
}

[[nodiscard]] auto test_env() -> AtspiEnv {
    AtspiEnv env;
    env.base_path = "/org/a11y/atspi/accessible/root";
    env.app_name = "probe-app";
    env.toolkit_version = "1.0.0";
    env.self_bus = ":1.42";
    env.window_title = "Probe Window";
    env.registry_root = AtspiRef{.bus = ":1.1", .path = k_registry_root_path};
    return env;
}

auto contains_str(const std::vector<std::string> &v, std::string_view s) -> bool {
    return std::ranges::any_of(v, [s](const std::string &e) { return e == s; });
}

}  // namespace

// ============================================================================
// 折算表
// ============================================================================

AURORA_TEST_CASE(role_table_pins_upstream_numbers) {
    // 序号是协议面（libatspi 按数字比对）；数值错一位 = 读屏全树念错。逐条钉死。
    const auto of = [](AccessibilityRole r, bool password = false) {
        AccessibilityNode n;
        n.role = r;
        n.state.password = password;
        return atspi_role_of(n);
    };
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Button), std::uint32_t{43});  // BUTTON（PUSH_BUTTON 是别名）
    AURORA_TEST_CHECK_NE(of(AccessibilityRole::Button), std::uint32_t{132});  // 防漂移：绝不落 PUSH_BUTTON_MENU 段
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::TextInput), std::uint32_t{79});  // ENTRY
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::TextInput, true), std::uint32_t{40});  // PASSWORD_TEXT
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Switch), std::uint32_t{130});  // SWITCH
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Checkbox), std::uint32_t{7});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Slider), std::uint32_t{51});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Progress), std::uint32_t{42});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Text), std::uint32_t{116});  // STATIC
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Image), std::uint32_t{27});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::List), std::uint32_t{31});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::ListItem), std::uint32_t{32});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Header), std::uint32_t{83});  // HEADING
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Dialog), std::uint32_t{16});
    AURORA_TEST_CHECK_EQ(of(AccessibilityRole::Generic), std::uint32_t{39});  // PANEL
}

AURORA_TEST_CASE(role_name_table_matches_libatspi_strings) {
    AURORA_TEST_CHECK_STREQ(atspi_role_name(43).c_str(), "push button");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(40).c_str(), "password text");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(116).c_str(), "static");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(130).c_str(), "switch");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(75).c_str(), "application");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(23).c_str(), "frame");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(67).c_str(), "unknown");
    AURORA_TEST_CHECK_STREQ(atspi_role_name(999).c_str(), "unknown");  // 越界兜底
}

AURORA_TEST_CASE(state_name_table_pins_event_minors) {
    // `object:state-changed:<name>` 的 minor 单一来源：AtspiStateType 序号 → 规范名
    // （小写连字符，GLib 枚举 nick 口径）。名字错位 = Orca 类型串匹配落空，事件静默丢失。
    AURORA_TEST_CHECK_STREQ(atspi_state_name(0), "invalid");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(1), "active");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(4), "checked");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(6), "defunct");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(8), "enabled");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(11), "focusable");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(12), "focused");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(17), "multi-line");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(20), "pressed");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(23), "selected");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(26), "single-line");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(30), "visible");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(38), "selectable-text");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(41), "checkable");
    AURORA_TEST_CHECK_STREQ(atspi_state_name(43), "read-only");
    AURORA_TEST_CHECK_TRUE(atspi_state_name(44) == nullptr);  // LAST_DEFINED 起越界
    AURORA_TEST_CHECK_TRUE(atspi_state_name(999) == nullptr);  // 越界兜底：不发无名事件
    // 本桥全部可申报状态都必须有名（发射器按 nullptr 静默丢弃 ⇒ 空名 = 事件漏发）。
    for (const std::uint32_t s :
         {state_enabled, state_sensitive, state_showing, state_visible, state_focusable, state_editable,
          state_multi_line, state_single_line, state_selectable_text, state_read_only, state_defunct}) {
        AURORA_TEST_CHECK_NOT_NULL(atspi_state_name(s));
    }
}

/// @brief 手搓节点补默认动作集（真实建树时 `build_accessibility_node` 自动填，见 accessibility.h）。
[[nodiscard]] auto with_actions(AccessibilityNode n) -> AccessibilityNode {
    n.actions = default_actions(n.role);
    return n;
}

AURORA_TEST_CASE(interfaces_derivation) {
    AccessibilityNode btn = with_actions(AccessibilityNode{.role = AccessibilityRole::Button});  // Click|Invoke|Focus
    auto ifaces = atspi_interfaces_of(btn);
    AURORA_TEST_CHECK_TRUE(contains_str(ifaces, k_iface_accessible));
    AURORA_TEST_CHECK_TRUE(contains_str(ifaces, k_iface_component));
    AURORA_TEST_CHECK_TRUE(contains_str(ifaces, k_iface_action));
    AURORA_TEST_CHECK_FALSE(contains_str(ifaces, k_iface_text));

    AccessibilityNode txt = with_actions(AccessibilityNode{.role = AccessibilityRole::TextInput});
    AURORA_TEST_CHECK_TRUE(contains_str(atspi_interfaces_of(txt), k_iface_text));
    // TextInput 的 Value 动作不产出 Action 行（AT-SPI 设值走 Value 接口）：无 Action 接口。
    AURORA_TEST_CHECK_FALSE(contains_str(atspi_interfaces_of(txt), k_iface_action));

    AccessibilityNode slider = with_actions(AccessibilityNode{.role = AccessibilityRole::Slider});
    slider.range = AccessibilityRange{};
    AURORA_TEST_CHECK_TRUE(contains_str(atspi_interfaces_of(slider), k_iface_value));

    AccessibilityNode label = with_actions(AccessibilityNode{.role = AccessibilityRole::Text});
    label.range = AccessibilityRange{};
    AURORA_TEST_CHECK_TRUE(contains_str(atspi_interfaces_of(label), k_iface_text));
}

AURORA_TEST_CASE(states_table) {
    AccessibilityNode n = with_actions(AccessibilityNode{.role = AccessibilityRole::Button});
    n.state.visible = true;
    auto s = atspi_states_of(n, atspi_interfaces_of(n));
    AURORA_TEST_CHECK_TRUE(std::ranges::includes(
        s, std::vector<std::uint32_t>{state_enabled, state_sensitive, state_showing, state_visible}));
    auto sorted = s;
    std::ranges::sort(sorted);
    AURORA_TEST_CHECK_TRUE(s == sorted);  // 输出升序（比对稳定）

    n.state.disabled = true;
    const auto ds = atspi_states_of(n, atspi_interfaces_of(n));
    AURORA_TEST_CHECK_FALSE(std::ranges::find(ds, state_enabled) != ds.end());  // 禁用 ⇒ 抽掉 ENABLED
    AURORA_TEST_CHECK_FALSE(std::ranges::find(ds, state_sensitive) != ds.end());  // 无 DISABLED 位（atk 口径）

    n.state.disabled = false;
    n.state.offscreen = true;
    const auto os = atspi_states_of(n, atspi_interfaces_of(n));
    AURORA_TEST_CHECK_FALSE(std::ranges::find(os, state_showing) != os.end());  // 离屏 ⇒ 无 SHOWING/VISIBLE

    // Text 接口附加位：可选中 + SELECTABLE_TEXT + 可编辑/只读 + 单行/多行。
    AccessibilityNode t;
    t.role = AccessibilityRole::TextInput;
    const auto ts = atspi_states_of(t, atspi_interfaces_of(t));
    AURORA_TEST_CHECK_TRUE(std::ranges::find(ts, state_selectable_text) != ts.end());
    AURORA_TEST_CHECK_TRUE(std::ranges::find(ts, state_editable) != ts.end());
    AURORA_TEST_CHECK_TRUE(std::ranges::find(ts, state_single_line) != ts.end());
    t.state.read_only = true;
    t.state.multiline = true;
    const auto rs = atspi_states_of(t, atspi_interfaces_of(t));
    AURORA_TEST_CHECK_TRUE(std::ranges::find(rs, state_read_only) != rs.end());
    AURORA_TEST_CHECK_FALSE(std::ranges::find(rs, state_editable) != rs.end());
    AURORA_TEST_CHECK_TRUE(std::ranges::find(rs, state_multi_line) != rs.end());
}

AURORA_TEST_CASE(actions_order_and_dedupe) {
    AccessibilityNode n = with_actions(AccessibilityNode{.role = AccessibilityRole::Button});  // Click|Invoke|Focus
    const auto press_only = atspi_actions_of(n);
    AURORA_TEST_REQUIRE_EQ(press_only.size(), std::size_t{1});  // Click+Invoke 去重为一个 press
    AURORA_TEST_CHECK_STREQ(press_only[0].name.c_str(), "press");
    AURORA_TEST_CHECK_TRUE(press_only[0].action == AccessibilityAction::Click);  // Click 优先于 Invoke

    n.actions = n.actions | AccessibilityAction::Toggle | AccessibilityAction::Select | AccessibilityAction::ScrollUp |
                AccessibilityAction::ScrollDown | AccessibilityAction::ScrollLeft | AccessibilityAction::ScrollRight |
                AccessibilityAction::ScrollIntoView;
    std::vector<std::string> names;
    for (const auto &row : atspi_actions_of(n)) {
        names.push_back(row.name);
    }
    const std::vector<std::string> want{"press",       "toggle",      "select",       "scroll up",
                                        "scroll down", "scroll left", "scroll right", "scroll to visible"};
    AURORA_TEST_CHECK_TRUE(names == want);

    // Focus 不算 Action 行（无「focus」动作名；纯容器不暴露 Action 接口）。
    AccessibilityNode g;
    g.role = AccessibilityRole::Generic;
    AURORA_TEST_CHECK_TRUE(atspi_actions_of(g).empty());
}

// ============================================================================
// 码点偏移
// ============================================================================

AURORA_TEST_CASE(cp_count_and_slice) {
    AURORA_TEST_CHECK_EQ(atspi_cp_count("abc"), std::size_t{3});
    AURORA_TEST_CHECK_EQ(atspi_cp_count("héllo"), std::size_t{5});
    AURORA_TEST_CHECK_EQ(atspi_cp_count("a😀b"), std::size_t{3});  // 4 字节 emoji 记 1 码点
    AURORA_TEST_CHECK_EQ(atspi_cp_count(""), std::size_t{0});

    // GetText(0,-1) = 全文（客户端普遍用法）。
    AURORA_TEST_CHECK_STREQ(atspi_cp_slice("héllo", 0, -1).c_str(), "héllo");
    AURORA_TEST_CHECK_STREQ(atspi_cp_slice("héllo", 1, 3).c_str(), "él");
    AURORA_TEST_CHECK_STREQ(atspi_cp_slice("a😀b", 1, 2).c_str(), "😀");  // 整码点切片不劈开多字节
    AURORA_TEST_CHECK_STREQ(atspi_cp_slice("abc", 1, 99).c_str(), "bc");  // 终点越界 ⇒ 夹紧到文末
    AURORA_TEST_CHECK_TRUE(atspi_cp_slice("abc", 5, 9).empty());  // 起点越界 ⇒ 空
    AURORA_TEST_CHECK_STREQ(atspi_cp_slice("abc", -1, -1).c_str(), "abc");  // 负起点夹紧 0
    AURORA_TEST_CHECK_TRUE(atspi_cp_slice("abc", 2, 2).empty());  // 空区间
    AURORA_TEST_CHECK_TRUE(atspi_cp_slice("", 0, -1).empty());
}

// ============================================================================
// AtspiModel：寻址 / 裁剪 / 路径稳定
// ============================================================================

AURORA_TEST_CASE(model_synthesizes_app_and_frame) {
    TreeSnapshot snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "ok"),
    });
    AtspiModel model{test_env()};
    model.sync(snap);

    AURORA_TEST_CHECK_TRUE(model.exists(AURORA_ATSPI_APP_ID));
    AURORA_TEST_CHECK_TRUE(model.exists(AURORA_ATSPI_FRAME_ID));
    AURORA_TEST_CHECK_EQ(model.role(AURORA_ATSPI_APP_ID), std::uint32_t{75});  // APPLICATION
    AURORA_TEST_CHECK_EQ(model.role(AURORA_ATSPI_FRAME_ID), std::uint32_t{23});  // FRAME
    AURORA_TEST_CHECK_STREQ(model.name(AURORA_ATSPI_APP_ID).c_str(), "probe-app");
    AURORA_TEST_CHECK_STREQ(model.name(AURORA_ATSPI_FRAME_ID).c_str(), "Probe Window");
    AURORA_TEST_CHECK_EQ(model.child_count(AURORA_ATSPI_APP_ID), std::int32_t{1});
    AURORA_TEST_CHECK_EQ(model.child_at(AURORA_ATSPI_APP_ID, 0).value_or(0), AURORA_ATSPI_FRAME_ID);
    AURORA_TEST_CHECK_EQ(model.parent(AURORA_ATSPI_FRAME_ID), AURORA_ATSPI_APP_ID);
    // 快照根（parent_id=0）挂 Frame 下；Frame 的父 = 注册表根。
    AURORA_TEST_CHECK_EQ(model.parent(10), AURORA_ATSPI_FRAME_ID);
    AURORA_TEST_CHECK_STREQ(model.parent_ref(AURORA_ATSPI_APP_ID).bus.c_str(), ":1.1");
    AURORA_TEST_CHECK_STREQ(model.path_of_id(AURORA_ATSPI_APP_ID).c_str(), "/org/a11y/atspi/accessible/root");
    AURORA_TEST_CHECK_EQ(model.index_in_parent(10), std::int32_t{0});
    AURORA_TEST_CHECK_EQ(model.index_in_parent(AURORA_ATSPI_APP_ID), std::int32_t{-1});
    // 未知路径 ⇒ 无 id；未知 id ⇒ null 路径 + defunct 状态。
    AURORA_TEST_CHECK_FALSE(model.id_of_path("/no/such/path").has_value());
    AURORA_TEST_CHECK_EQ(model.id_of_path("/org/a11y/atspi/accessible/root/1").value_or(0), std::uint64_t{10});
    AURORA_TEST_CHECK_STREQ(model.path_of_id(9999).c_str(), k_null_path);
    const auto dead_states = model.states(9999);
    AURORA_TEST_CHECK_TRUE(dead_states.size() == 1 && dead_states[0] == state_defunct);
}

AURORA_TEST_CASE(model_prunes_decoration_and_promotes_children) {
    // 20 为装饰节点（!is_control && !is_content）：不入存活表，其子 21 上挂到 10。
    TreeSnapshot snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(20, 10, AccessibilityRole::Image, ""),
        leaf(21, 20, AccessibilityRole::Button, "deep"),
    });
    snap.flat[1].node.is_control = false;
    snap.flat[1].node.is_content = false;

    AtspiModel model{test_env()};
    model.sync(snap);

    AURORA_TEST_CHECK_FALSE(model.exists(20));
    AURORA_TEST_CHECK_TRUE(model.exists(21));
    AURORA_TEST_CHECK_EQ(model.parent(21), std::uint64_t{10});
    AURORA_TEST_CHECK_EQ(model.child_count(10), std::int32_t{1});
    AURORA_TEST_CHECK_EQ(model.index_in_parent(21), std::int32_t{0});
    AURORA_TEST_CHECK_EQ(model.id_of_path(model.path_of_id(21)).value_or(0), std::uint64_t{21});
}

AURORA_TEST_CASE(model_paths_stable_across_syncs) {
    auto snap_a = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "a"),
        leaf(12, 10, AccessibilityRole::Button, "b"),
        leaf(13, 10, AccessibilityRole::Button, "c"),
    });
    AtspiModel model{test_env()};
    model.sync(snap_a);
    const std::string pa = model.path_of_id(11);
    const std::string pb = model.path_of_id(12);
    const std::string pc = model.path_of_id(13);

    // b 销毁、d 新增：a/c 路径不变（客户端手里的引用不串号），d 拿新分配号。
    auto snap_b = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "a"),
        leaf(13, 10, AccessibilityRole::Button, "c"),
        leaf(14, 10, AccessibilityRole::Button, "d"),
    });
    model.sync(snap_b);
    AURORA_TEST_CHECK_STREQ(model.path_of_id(11).c_str(), pa.c_str());
    AURORA_TEST_CHECK_STREQ(model.path_of_id(13).c_str(), pc.c_str());
    AURORA_TEST_CHECK_FALSE(model.exists(12));
    AURORA_TEST_CHECK_NE(model.path_of_id(14), pb);  // 不复用销毁路径
    AURORA_TEST_CHECK_TRUE(model.path_of_id(14).starts_with("/org/a11y/atspi/accessible/root/"));
}

// ============================================================================
// AtspiModel：几何 / 文本 / 取值 / 动作
// ============================================================================

AURORA_TEST_CASE(model_geometry_and_hit_test) {
    TreeSnapshot snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "btn"),
    });
    snap.flat[0].node.bounds = box(0, 0, 200, 100);
    snap.flat[1].node.bounds = box(50.5F, 20.25F, 40, 16);

    AtspiModel model{test_env()};
    model.sync(snap);

    const auto r = model.extents(11, coord_screen);
    AURORA_TEST_CHECK_EQ(r.x, std::int32_t{50});  // 原点向下取整
    AURORA_TEST_CHECK_EQ(r.y, std::int32_t{20});
    AURORA_TEST_CHECK_EQ(r.width, std::int32_t{41});  // 终点向上取整（90.5→91）
    AURORA_TEST_CHECK_EQ(r.height, std::int32_t{17});  // 终点向上取整（36.25→37 − 20）
    AURORA_TEST_CHECK_TRUE(model.contains(11, 60, 25, coord_screen));
    AURORA_TEST_CHECK_FALSE(model.contains(11, 49, 25, coord_screen));
    // 命中取先序逆序最深者：btn 盒内的点不落给容器；容器内空点落给容器本身。
    AURORA_TEST_CHECK_EQ(model.accessible_at_point(10, 60, 25, coord_screen).path, model.path_of_id(11));
    AURORA_TEST_CHECK_EQ(model.accessible_at_point(10, 5, 5, coord_screen).path, model.path_of_id(10));
    // WINDOW 系：屏幕盒平移回窗口原点（origin 缺省 0 ⇒ 同值）。
    const auto rw = model.extents(11, coord_window);
    AURORA_TEST_CHECK_EQ(rw.x, r.x);
}

AURORA_TEST_CASE(model_text_value_action) {
    ProbeWidget w{"héllo"};
    w.selection = AccessibilityTextSelection{.start = 3, .end = 3};  // 字节 3 = 码点 2（'é' 后）

    TreeSnapshot snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::TextInput, "input", &w),
    });

    int performs = 0;
    AccessibilityAction last_action = AccessibilityAction::None;
    double last_number = 0.0;
    AtspiEnv env = test_env();
    env.perform = [&](Widget *, const AccessibilityActionRequest &req) {
        ++performs;
        last_action = req.action;
        last_number = req.number;
        return true;
    };
    AtspiModel model{std::move(env)};
    model.sync(snap);

    AURORA_TEST_CHECK_TRUE(model.has_text(11));
    AURORA_TEST_CHECK_EQ(model.text_char_count(11), std::int32_t{5});  // 码点数，非字节数（6 字节）
    AURORA_TEST_CHECK_STREQ(model.text_slice(11, 0, -1).c_str(), "héllo");
    AURORA_TEST_CHECK_STREQ(model.text_slice(11, 1, 3).c_str(), "él");
    AURORA_TEST_CHECK_EQ(model.text_caret(11), std::int32_t{2});  // 字节 3 → 码点 2
    AURORA_TEST_CHECK_FALSE(model.has_text(10));  // 容器无 Text 接口
    AURORA_TEST_CHECK_EQ(model.text_char_count(9999), std::int32_t{0});

    // Value 接口：range 存在才有；设值经 env.perform 转 Value 动作。
    snap.flat[1].node.range = AccessibilityRange{.min = 0, .max = 10, .step = 0.5, .value = 3};
    model.sync(snap);
    AURORA_TEST_CHECK_TRUE(model.has_value(11));
    AURORA_TEST_CHECK_NEAR(model.value_max(11), 10.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(model.value_current(11), 3.0, 1e-9);
    AURORA_TEST_CHECK_TRUE(model.value_set(11, 7.5));
    AURORA_TEST_CHECK_TRUE(last_action == AccessibilityAction::Value);
    AURORA_TEST_CHECK_NEAR(last_number, 7.5, 1e-9);

    // Action 接口：TextInput 无动作行 ⇒ do_action 越界 false；Button 有 press。
    AURORA_TEST_CHECK_EQ(model.actions(11).size(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(model.do_action(11, 0));
    const std::uint64_t btn_id = 12;
    auto with_btn = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(btn_id, 10, AccessibilityRole::Button, "ok", &w),
    });
    model.sync(with_btn);
    AURORA_TEST_REQUIRE_EQ(model.actions(btn_id).size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(model.do_action(btn_id, 0));
    AURORA_TEST_CHECK_TRUE(last_action == AccessibilityAction::Click);
    AURORA_TEST_CHECK_EQ(performs, 2);
    AURORA_TEST_CHECK_FALSE(model.do_action(btn_id, 5));  // 越界
}

// ============================================================================
// AtspiModel：Properties / Cache / handles
// ============================================================================

AURORA_TEST_CASE(model_properties_get_set) {
    auto snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "ok"),
    });
    snap.flat[1].node.range = AccessibilityRange{.min = 0, .max = 1, .step = 1, .value = 0.5};
    AtspiModel model{test_env()};
    model.sync(snap);

    // Application 接口只认 App 根。
    const auto tk = model.prop_get(AURORA_ATSPI_APP_ID, k_iface_application, "ToolkitName");
    AURORA_TEST_CHECK_TRUE(tk.kind == AtspiPropValue::Kind::Str);
    AURORA_TEST_CHECK_STREQ(tk.str.c_str(), "Aurora");
    AURORA_TEST_CHECK_TRUE(model.prop_get(11, k_iface_application, "ToolkitName").kind == AtspiPropValue::Kind::None);

    AURORA_TEST_CHECK_TRUE(model.prop_get(AURORA_ATSPI_APP_ID, k_iface_application, "Id").kind ==
                           AtspiPropValue::Kind::I32);
    AURORA_TEST_CHECK_EQ(model.prop_get(AURORA_ATSPI_APP_ID, k_iface_application, "Id").i32, std::int32_t{-1});
    AtspiPropValue id_v;
    id_v.kind = AtspiPropValue::Kind::I32;
    id_v.i32 = 77;
    AURORA_TEST_CHECK_TRUE(model.prop_set(AURORA_ATSPI_APP_ID, k_iface_application, "Id", id_v));
    AURORA_TEST_CHECK_EQ(model.app_id(), std::int32_t{77});
    AURORA_TEST_CHECK_EQ(model.prop_get(AURORA_ATSPI_APP_ID, k_iface_application, "Id").i32, std::int32_t{77});
    // 非 App 节点 / 非 I32 的 Set 拒绝（桥回 InvalidArgs）。
    AURORA_TEST_CHECK_FALSE(model.prop_set(11, k_iface_application, "Id", id_v));
    id_v.kind = AtspiPropValue::Kind::Str;
    AURORA_TEST_CHECK_FALSE(model.prop_set(AURORA_ATSPI_APP_ID, k_iface_application, "Id", id_v));

    AURORA_TEST_CHECK_STREQ(model.prop_get(11, k_iface_accessible, "Name").str.c_str(), "ok");
    AURORA_TEST_CHECK_EQ(model.prop_get(11, k_iface_accessible, "ChildCount").i32, std::int32_t{0});
    const auto parent_v = model.prop_get(11, k_iface_accessible, "Parent");
    AURORA_TEST_CHECK_TRUE(parent_v.kind == AtspiPropValue::Kind::Ref);
    AURORA_TEST_CHECK_STREQ(parent_v.ref.bus.c_str(), ":1.42");
    AURORA_TEST_CHECK_STREQ(parent_v.ref.path.c_str(), model.path_of_id(10).c_str());
    AURORA_TEST_CHECK_TRUE(model.prop_get(11, k_iface_accessible, "version").kind == AtspiPropValue::Kind::U32);
    AURORA_TEST_CHECK_EQ(model.prop_get(11, k_iface_accessible, "version").u32, std::uint32_t{2});
    AURORA_TEST_CHECK_TRUE(model.prop_get(11, k_iface_accessible, "NoSuch").kind == AtspiPropValue::Kind::None);

    // Value.CurrentValue 读写（widget 为空 ⇒ Set 走 false 分支但 Get 正常）。
    AURORA_TEST_CHECK_NEAR(model.prop_get(11, k_iface_value, "CurrentValue").dbl, 0.5, 1e-9);
    AtspiPropValue dv;
    dv.kind = AtspiPropValue::Kind::Dbl;
    dv.dbl = 1.0;
    AURORA_TEST_CHECK_FALSE(model.prop_set(11, k_iface_value, "CurrentValue", dv));  // 无活控件 ⇒ 设值失败

    AURORA_TEST_CHECK_TRUE(model.prop_get(11, "org.freedesktop.DBus.Foo", "Bar").kind == AtspiPropValue::Kind::None);
}

AURORA_TEST_CASE(model_cache_rows_shape) {
    TreeSnapshot snap = snapshot({
        leaf(10, 0, AccessibilityRole::Generic, "root"),
        leaf(11, 10, AccessibilityRole::Button, "ok"),
    });
    snap.flat[0].node.bounds = box(0, 0, 100, 50);
    AtspiModel model{test_env()};
    model.sync(snap);

    const auto rows = model.cache_rows();
    AURORA_TEST_REQUIRE_EQ(rows.size(), std::size_t{4});  // App + Frame + root + btn
    const AtspiCacheRow &app = rows[0];
    AURORA_TEST_CHECK_STREQ(app.self.path.c_str(), "/org/a11y/atspi/accessible/root");
    AURORA_TEST_CHECK_EQ(app.role, std::uint32_t{75});
    AURORA_TEST_CHECK_TRUE(app.parent.is_null());  // application 行父 = null 引用
    AURORA_TEST_CHECK_EQ(app.index_in_parent, std::int32_t{-1});
    AURORA_TEST_CHECK_TRUE(contains_str(app.interfaces, k_iface_application));
    const AtspiCacheRow &btn = rows[3];
    AURORA_TEST_CHECK_STREQ(btn.self.bus.c_str(), ":1.42");
    AURORA_TEST_CHECK_STREQ(btn.app.path.c_str(), app.self.path.c_str());
    AURORA_TEST_CHECK_STREQ(btn.parent.path.c_str(), model.path_of_id(10).c_str());
    AURORA_TEST_CHECK_EQ(btn.role, std::uint32_t{43});
    AURORA_TEST_CHECK_STREQ(btn.name.c_str(), "ok");
    AURORA_TEST_CHECK_EQ(btn.index_in_parent, std::int32_t{0});
    AURORA_TEST_CHECK_TRUE(contains_str(btn.interfaces, k_iface_action));
    // 状态表升序且含 ENABLED。
    AURORA_TEST_CHECK_TRUE(std::ranges::is_sorted(btn.states));
    AURORA_TEST_CHECK_TRUE(std::ranges::find(btn.states, state_enabled) != btn.states.end());
}

AURORA_TEST_CASE(handles_member_table) {
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_accessible, "GetRole"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_accessible, "GetChildren"));
    AURORA_TEST_CHECK_FALSE(AtspiModel::handles(k_iface_accessible, "GetExtents"));  // 属 Component
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_component, "GetExtents"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_text, "GetText"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_action, "DoAction"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_cache, "GetItems"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles(k_iface_socket, "Embed"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles("org.freedesktop.DBus.Properties", "Get"));
    AURORA_TEST_CHECK_TRUE(AtspiModel::handles("org.freedesktop.DBus.Introspectable", "Introspect"));
    AURORA_TEST_CHECK_FALSE(AtspiModel::handles("org.example.Bogus", "Get"));
    AURORA_TEST_CHECK_FALSE(AtspiModel::handles(k_iface_value, "SetValue"));  // Value 纯 Properties
}

}  // namespace aurora::test_cases::utest_atspi_protocol
