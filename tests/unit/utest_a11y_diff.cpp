/// 测试类型: unit
/// 目标单元: include/aurora/widget/a11y_diff.h
/// 测试说明: 语义树快照扁平化（先序 / parent_id / children_of）、diff 的 add/remove/move/
///           字段级 updated（Name/Value/Hint/Bounds/Range/Actions/State）与焦点位变化，
///           以及 LCS 换序判定优先于 remove+add（读屏焦点稳定性）

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/a11y_types.h"
#include "aurora/core/accessibility.h"
#include "aurora/widget/a11y_diff.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_a11y_diff {

namespace {

using aurora::a11y::FieldChange;
using aurora::a11y::NodeSnapshot;
using aurora::a11y::TreeDiff;
using aurora::a11y::TreeSnapshot;

/// @brief 纯数据构造一个快照节点（不依赖控件，专测 diff 这一纯函数）。
[[nodiscard]] auto node(std::uint64_t id, std::uint64_t parent_id, std::string name) -> NodeSnapshot {
    NodeSnapshot n;
    n.id = id;
    n.parent_id = parent_id;
    n.node.id = id;
    n.node.name = std::move(name);
    return n;
}

/// @brief 把若干节点装入快照（顺带重建 by_id / children_of 索引）。
[[nodiscard]] auto snapshot(std::vector<NodeSnapshot> flat) -> TreeSnapshot {
    TreeSnapshot snap;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        snap.by_id[flat[i].id] = i;
        snap.children_of[flat[i].parent_id].push_back(flat[i].id);
    }
    snap.flat = std::move(flat);
    return snap;
}

[[nodiscard]] auto has_field(const TreeDiff &d, std::uint64_t id, FieldChange f) -> bool {
    return std::ranges::any_of(d.updated, [&](const std::pair<std::uint64_t, FieldChange> &e) -> bool {
        return e.first == id && e.second == f;
    });
}

// ---- 真实控件（验 flatten 与控件树同序）----

class ProbeLeaf final : public LeafWidget {
  public:
    explicit ProbeLeaf(const char *type) : type_{type} {}
    [[nodiscard]] auto type_name() const -> const char * override { return type_; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override { return {}; }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    const char *type_;
};

class ProbeColumn final : public Container {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "Column"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override { return {}; }
};

}  // namespace

AURORA_TEST_CASE(flatten_is_preorder_with_parent_links) {
    ProbeColumn root;
    const auto a = std::make_shared<ProbeLeaf>("Button");
    const auto b = std::make_shared<ProbeLeaf>("Text");
    root.add(Node{ProbeLeaf{"Header"}});
    root.add(Node{a});
    root.add(Node{b});

    const TreeSnapshot snap = aurora::a11y::build_tree_snapshot(root);
    AURORA_TEST_REQUIRE_EQ(snap.flat.size(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(snap.flat.front().id, root.runtime_id());  // 先序：根在最前
    AURORA_TEST_CHECK_EQ(snap.flat.front().parent_id, std::uint64_t{0});

    const std::vector<std::uint64_t> &kids = snap.children_of.at(root.runtime_id());
    AURORA_TEST_REQUIRE_EQ(kids.size(), std::size_t{3});
    for (const NodeSnapshot &n : snap.flat) {
        if (n.parent_id != 0) {
            AURORA_TEST_CHECK_EQ(n.parent_id, root.runtime_id());
        }
        AURORA_TEST_CHECK_NOT_NULL(snap.find(n.id));
    }
    AURORA_TEST_CHECK_NULL(snap.find(0xDEADBEEFU));  // 未命中 → nullptr
}

AURORA_TEST_CASE(identical_snapshots_produce_empty_diff) {
    const TreeSnapshot a = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 1, "b")});
    const TreeSnapshot b = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 1, "b")});
    AURORA_TEST_CHECK_TRUE(aurora::a11y::diff_snapshots(a, b).empty());
}

AURORA_TEST_CASE(added_and_removed_nodes_are_reported) {
    const TreeSnapshot before = snapshot({node(1, 0, "root"), node(2, 1, "a")});
    const TreeSnapshot after = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 1, "b")});
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_REQUIRE_EQ(d.added.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(d.added.front(), std::uint64_t{3});
    AURORA_TEST_CHECK_TRUE(d.removed.empty());

    const TreeDiff back = aurora::a11y::diff_snapshots(after, before);
    AURORA_TEST_REQUIRE_EQ(back.removed.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(back.removed.front(), std::uint64_t{3});
    AURORA_TEST_CHECK_TRUE(back.added.empty());
}

AURORA_TEST_CASE(reorder_is_moved_not_remove_plus_add) {
    // 读屏焦点稳定性优先：同父换序只报 moved，绝不退化成 remove+add（会打断焦点）。
    const TreeSnapshot before = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 1, "b")});
    const TreeSnapshot after = snapshot({node(1, 0, "root"), node(3, 1, "b"), node(2, 1, "a")});
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_CHECK_TRUE(d.added.empty());
    AURORA_TEST_CHECK_TRUE(d.removed.empty());
    AURORA_TEST_CHECK_FALSE(d.moved.empty());
}

AURORA_TEST_CASE(reparent_is_moved_and_deduplicated) {
    const TreeSnapshot before = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 1, "b")});
    const TreeSnapshot after = snapshot({node(1, 0, "root"), node(2, 1, "a"), node(3, 2, "b")});
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_REQUIRE_EQ(d.moved.size(), std::size_t{1});  // 去重重：换父 + 换序只报一次
    AURORA_TEST_CHECK_EQ(d.moved.front(), std::uint64_t{3});
}

AURORA_TEST_CASE(name_value_hint_changes_are_field_level) {
    TreeSnapshot before = snapshot({node(1, 0, "root")});
    TreeSnapshot after = snapshot({node(1, 0, "root2")});
    after.flat[0].node.value = "v";
    after.flat[0].node.hint = "h";
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::Name));
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::Value));
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::Hint));
}

AURORA_TEST_CASE(bounds_change_is_reported) {
    TreeSnapshot before = snapshot({node(1, 0, "root")});
    TreeSnapshot after = snapshot({node(1, 0, "root")});
    after.flat[0].node.bounds.size.width = 42.0F;
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::Bounds));
}

AURORA_TEST_CASE(range_appear_and_value_change_are_reported) {
    TreeSnapshot before = snapshot({node(1, 0, "root")});
    TreeSnapshot after = snapshot({node(1, 0, "root")});
    after.flat[0].node.range = AccessibilityRange{.min = 0.0, .max = 10.0, .step = 1.0, .value = 3.0};
    AURORA_TEST_CHECK_TRUE(has_field(aurora::a11y::diff_snapshots(before, after), 1, FieldChange::Range));

    TreeSnapshot after2 = snapshot({node(1, 0, "root")});
    after2.flat[0].node.range = AccessibilityRange{.min = 0.0, .max = 10.0, .step = 1.0, .value = 7.0};
    AURORA_TEST_CHECK_TRUE(has_field(aurora::a11y::diff_snapshots(after, after2), 1, FieldChange::Range));
}

AURORA_TEST_CASE(state_change_is_reported) {
    TreeSnapshot before = snapshot({node(1, 0, "root")});
    TreeSnapshot after = snapshot({node(1, 0, "root")});
    after.flat[0].node.state.checked = true;
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::State));
}

AURORA_TEST_CASE(actions_change_is_reported) {
    TreeSnapshot before = snapshot({node(1, 0, "root")});
    TreeSnapshot after = snapshot({node(1, 0, "root")});
    after.flat[0].node.actions = AccessibilityAction::Click;
    const TreeDiff d = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_CHECK_TRUE(has_field(d, 1, FieldChange::Actions));
}

AURORA_TEST_CASE(focus_gain_is_reported_once_and_loss_is_not) {
    TreeSnapshot before = snapshot({node(1, 0, "root"), node(2, 1, "a")});
    TreeSnapshot after = snapshot({node(1, 0, "root"), node(2, 1, "a")});
    after.flat[1].node.state.focused = true;

    const TreeDiff gain = aurora::a11y::diff_snapshots(before, after);
    AURORA_TEST_REQUIRE_TRUE(gain.focused_id.has_value());
    AURORA_TEST_CHECK_EQ(*gain.focused_id, std::uint64_t{2});

    const TreeDiff loss = aurora::a11y::diff_snapshots(after, before);
    AURORA_TEST_CHECK_FALSE(loss.focused_id.has_value());  // 失焦不产 focused_id
    AURORA_TEST_CHECK_TRUE(has_field(loss, 2, FieldChange::State));
}

}  // namespace aurora::test_cases::utest_a11y_diff
