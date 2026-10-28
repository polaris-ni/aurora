/// 测试类型: unit
/// 目标单元: include/aurora/state/state_graph.h
/// 测试说明: StateGraph 的节点与读取边契约——活来源按角色报告为 `source` / `derived`，终端运行记录
/// 报告为 `terminal`，边一律为观察者 → 依赖来源的 `reads`；节点 id 取引擎稳定身份（复制句柄不新增
/// 节点），凭证取消或对象析构后陈旧条目被过滤；JSON 带 `schema: 1`，文本首行同口径且与自由函数等价
/// 覆盖说明: 用例经 `detail::HandleAccess::source_of` 与 `detail::live_terminals` 把自建句柄关联到
/// 图节点 id；这是测试对内部登记面的刻意依赖，公共契约只承诺 id 稳定与形态

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurora/state/computed.h"
#include "aurora/state/detail/registry.h"
#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_graph.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_state_graph {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）
using aurora::testing::require_child;
using aurora::testing::require_field;

/// @brief 来源节点的图 id：与 `StateGraph` 同口径（`s` 前缀 + 稳定身份）。
[[nodiscard]] auto source_id(const detail::SourceNode &node) -> std::string { return "s" + std::to_string(node.id()); }

/// @brief 终端节点的图 id：与 `StateGraph` 同口径（`t` 前缀 + 登记序号）。
[[nodiscard]] auto terminal_id(const detail::RunRecord &record) -> std::string {
    return "t" + std::to_string(record.seq());
}

[[nodiscard]] auto find_node(const std::vector<StateGraph::Node> &nodes, const std::string &id)
    -> const StateGraph::Node * {
    for (const auto &node : nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

[[nodiscard]] auto has_node(const std::vector<StateGraph::Node> &nodes, const std::string &id) -> bool {
    return find_node(nodes, id) != nullptr;
}

[[nodiscard]] auto node_kind_is(const std::vector<StateGraph::Node> &nodes, const std::string &id,
                                const std::string &kind) -> bool {
    const auto *node = find_node(nodes, id);
    return node != nullptr && node->kind == kind;
}

[[nodiscard]] auto has_edge(const std::vector<StateGraph::Edge> &edges, const std::string &from, const std::string &to,
                            const std::string &kind) -> bool {
    return std::ranges::any_of(edges, [&](const StateGraph::Edge &edge) -> bool {
        return edge.from == from && edge.to == to && edge.kind == kind;
    });
}

[[nodiscard]] auto count_kind(const std::vector<StateGraph::Node> &nodes, const std::string &kind) -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(nodes, [&](const StateGraph::Node &node) -> bool { return node.kind == kind; }));
}

[[nodiscard]] auto count_edges(const std::vector<StateGraph::Edge> &edges, const std::string &from,
                               const std::string &to, const std::string &kind) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(edges, [&](const StateGraph::Edge &edge) -> bool {
        return edge.from == from && edge.to == to && edge.kind == kind;
    }));
}

[[nodiscard]] auto edge_references(const std::vector<StateGraph::Edge> &edges, const std::string &id) -> bool {
    return std::ranges::any_of(edges,
                               [&](const StateGraph::Edge &edge) -> bool { return edge.from == id || edge.to == id; });
}

/// @brief 找出读取了给定来源的存活终端（本用例刚建立的 `effect` 或订阅）。
/// @param source 目标来源节点。
/// @return 匹配的运行记录；没有读取该来源的终端时为空。
[[nodiscard]] auto find_terminal_reading(const detail::SourceNode &source) -> std::shared_ptr<detail::RunRecord> {
    for (const auto &record : detail::live_terminals()) {
        if (!record || record->cancelled()) {
            continue;
        }
        for (const auto &dependency : record->dependency_nodes()) {
            if (dependency.get() == &source) {
                return record;
            }
        }
    }
    return nullptr;
}

/// @brief 找出读取了给定来源的派生节点（本用例刚建立的 `computed`）。
/// @param source 目标来源节点。
/// @return 匹配的派生来源；没有读取该来源的派生时为空。
[[nodiscard]] auto find_derived_reading(const detail::SourceNode &source) -> std::shared_ptr<detail::SourceNode> {
    for (const auto &node : detail::live_sources()) {
        if (!node || node->role() != detail::SourceNode::NodeRole::Derived) {
            continue;
        }
        for (const auto &dependency : node->dependency_nodes()) {
            if (dependency.get() == &source) {
                return node;
            }
        }
    }
    return nullptr;
}

AURORA_TEST_CASE(live_state_appears_as_source_node) {
    // 活来源：构造即入登记表，按角色报告为 `source`，id 取稳定身份而非对象地址。
    const auto sources_before = count_kind(StateGraph::nodes(), "source");
    State<int> s{0};
    const auto id = source_id(*detail::HandleAccess::source_of(s));

    const auto nodes = StateGraph::nodes();
    AURORA_TEST_CHECK_EQ(count_kind(nodes, "source"), sources_before + 1);
    AURORA_TEST_CHECK_TRUE(node_kind_is(nodes, id, "source"));
}

AURORA_TEST_CASE(copied_handle_shares_one_node) {
    // 复制句柄共享来源：不新增节点；同一 id 在图上仍可见。
    State<int> s{1};
    const auto id = source_id(*detail::HandleAccess::source_of(s));
    const auto sources_before = count_kind(StateGraph::nodes(), "source");

    // 本用例刻意按值复制以验证「不新增节点」的共享语义，复制本身就是被测行为。
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    State<int> copy = s;
    AURORA_TEST_CHECK_EQ(copy.get(), 1);
    AURORA_TEST_CHECK_EQ(count_kind(StateGraph::nodes(), "source"), sources_before);
    AURORA_TEST_CHECK_TRUE(has_node(StateGraph::nodes(), id));
}

AURORA_TEST_CASE(effect_reads_state_and_reset_removes_terminal) {
    // 终端运行记录：effect 首跑读取登记 `terminal --reads--> source`；reset 后节点与边一起消失。
    State<int> s{0};
    const auto state_id = source_id(*detail::HandleAccess::source_of(s));
    const auto terminals_before = count_kind(StateGraph::nodes(), "terminal");

    auto watch = effect([&s]() -> void { (void)s.get(); });
    const auto record = find_terminal_reading(*detail::HandleAccess::source_of(s));
    AURORA_TEST_REQUIRE(record != nullptr);
    const auto terminal = terminal_id(*record);

    const auto nodes = StateGraph::nodes();
    AURORA_TEST_CHECK_EQ(count_kind(nodes, "terminal"), terminals_before + 1);
    AURORA_TEST_CHECK_TRUE(node_kind_is(nodes, terminal, "terminal"));
    AURORA_TEST_CHECK_TRUE(has_edge(StateGraph::edges(), terminal, state_id, "reads"));

    watch.reset();
    AURORA_TEST_CHECK_FALSE(has_node(StateGraph::nodes(), terminal));
    AURORA_TEST_CHECK_FALSE(edge_references(StateGraph::edges(), terminal));
}

AURORA_TEST_CASE(computed_reports_derived_node_and_reads_edge) {
    // 派生来源：惰性求值后才形成依赖边；节点按角色报告为 `derived`。
    State<int> s{2};
    const auto state_id = source_id(*detail::HandleAccess::source_of(s));

    const auto tripled = computed([&s]() -> int { return s.get() * 3; });
    AURORA_TEST_CHECK_EQ(tripled.get(), 6);  // 首次读取触发求值

    const auto node = find_derived_reading(*detail::HandleAccess::source_of(s));
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto derived_id = source_id(*node);
    AURORA_TEST_CHECK_TRUE(node_kind_is(StateGraph::nodes(), derived_id, "derived"));
    AURORA_TEST_CHECK_TRUE(has_edge(StateGraph::edges(), derived_id, state_id, "reads"));
}

AURORA_TEST_CASE(duplicate_reads_keep_a_single_edge) {
    // 同一来源在一轮求值 / 回调内重复读取只保留一条边：导出图与反向通知关系一致。
    State<int> s{3};
    const auto state_id = source_id(*detail::HandleAccess::source_of(s));
    auto doubled = computed([&s]() -> int { return s.get() + s.get(); });
    AURORA_TEST_CHECK_EQ(doubled.get(), 6);
    const auto node = find_derived_reading(*detail::HandleAccess::source_of(s));
    AURORA_TEST_REQUIRE(node != nullptr);
    const auto derived_id = source_id(*node);

    const auto watch = effect([&doubled]() -> void {
        (void)doubled.get();
        (void)doubled.get();
    });
    const auto record = find_terminal_reading(*node);
    AURORA_TEST_REQUIRE(record != nullptr);
    const auto terminal = terminal_id(*record);

    const auto edges = StateGraph::edges();
    AURORA_TEST_CHECK_EQ(count_edges(edges, derived_id, state_id, "reads"), std::size_t{1});
    AURORA_TEST_CHECK_EQ(count_edges(edges, terminal, derived_id, "reads"), std::size_t{1});

    // 单边不改变语义：上游一次写入照常刷新派生与终端。
    s.set(5);
    AURORA_TEST_CHECK_EQ(doubled.get(), 10);
}

AURORA_TEST_CASE(stale_entries_are_skipped_after_destruction) {
    // 登记表只持弱引用：来源与终端析构后，节点与引用它们的边都从快照中消失。
    std::string dead_state_id;
    std::string dead_terminal_id;
    {
        State<int> dead{0};
        dead_state_id = source_id(*detail::HandleAccess::source_of(dead));
        const auto watch = effect([&dead]() -> void { (void)dead.get(); });
        const auto record = find_terminal_reading(*detail::HandleAccess::source_of(dead));
        AURORA_TEST_REQUIRE(record != nullptr);
        dead_terminal_id = terminal_id(*record);
        AURORA_TEST_REQUIRE_TRUE(has_node(StateGraph::nodes(), dead_state_id));
        AURORA_TEST_REQUIRE_TRUE(has_edge(StateGraph::edges(), dead_terminal_id, dead_state_id, "reads"));
    }
    AURORA_TEST_CHECK_FALSE(has_node(StateGraph::nodes(), dead_state_id));
    AURORA_TEST_CHECK_FALSE(has_node(StateGraph::nodes(), dead_terminal_id));
    AURORA_TEST_CHECK_FALSE(edge_references(StateGraph::edges(), dead_state_id));
    AURORA_TEST_CHECK_FALSE(edge_references(StateGraph::edges(), dead_terminal_id));
}

AURORA_TEST_CASE(to_json_reports_nodes_and_reads_edges_shape) {
    // JSON 形态：{schema: 1, nodes:[{id,kind}], edges:[{from,to,kind}]}，边类别恒为 "reads"。
    State<int> s{0};
    const auto state_id = source_id(*detail::HandleAccess::source_of(s));
    const auto watch = effect([&s]() -> void { (void)s.get(); });
    const auto record = find_terminal_reading(*detail::HandleAccess::source_of(s));
    AURORA_TEST_REQUIRE(record != nullptr);
    const auto terminal = terminal_id(*record);

    const auto document = aurora::state_graph();
    AURORA_TEST_REQUIRE_TRUE(document.contains("schema"));
    AURORA_TEST_CHECK_EQ(require_field<std::int64_t>(document, "schema"), std::int64_t{1});
    AURORA_TEST_REQUIRE_TRUE(document.contains("nodes"));
    AURORA_TEST_REQUIRE_TRUE(require_child(document, "nodes")->is_array());
    AURORA_TEST_REQUIRE_TRUE(document.contains("edges"));
    AURORA_TEST_REQUIRE_TRUE(require_child(document, "edges")->is_array());

    bool saw_state_node = false;
    bool saw_terminal_node = false;
    const auto *const nodes = require_child(document, "nodes");
    for (const auto &item : *nodes) {
        AURORA_TEST_REQUIRE(item.contains("id"));
        AURORA_TEST_REQUIRE(item.contains("kind"));
        const auto id = require_field<std::string>(item, "id");
        if (id == state_id) {
            saw_state_node = true;
            AURORA_TEST_CHECK_EQ(require_field<std::string>(item, "kind"), std::string("source"));
        } else if (id == terminal) {
            saw_terminal_node = true;
            AURORA_TEST_CHECK_EQ(require_field<std::string>(item, "kind"), std::string("terminal"));
        }
    }
    AURORA_TEST_CHECK_TRUE(saw_state_node);
    AURORA_TEST_CHECK_TRUE(saw_terminal_node);

    bool saw_reads = false;
    const auto *const edges = require_child(document, "edges");
    for (const auto &item : *edges) {
        AURORA_TEST_REQUIRE(item.contains("from"));
        AURORA_TEST_REQUIRE(item.contains("to"));
        AURORA_TEST_REQUIRE(item.contains("kind"));
        if (require_field<std::string>(item, "from") == terminal &&
            require_field<std::string>(item, "to") == state_id) {
            saw_reads = true;
            AURORA_TEST_CHECK_EQ(require_field<std::string>(item, "kind"), std::string("reads"));
        }
    }
    AURORA_TEST_CHECK_TRUE(saw_reads);
}

AURORA_TEST_CASE(to_text_matches_json_and_free_function) {
    // 文本形态以 `StateGraph schema=1:` 起头，逐行列出 `[kind] id` 与 `from --reads--> to` 边；
    // 自由函数与静态成员等价，行数与 JSON 的数组长度一致。
    State<int> s{0};
    const auto state_id = source_id(*detail::HandleAccess::source_of(s));
    const auto watch = effect([&s]() -> void { (void)s.get(); });
    const auto record = find_terminal_reading(*detail::HandleAccess::source_of(s));
    AURORA_TEST_REQUIRE(record != nullptr);
    const auto terminal = terminal_id(*record);

    const auto text = StateGraph::to_text();
    AURORA_TEST_CHECK_THAT(text, m::starts_with("StateGraph schema=1:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[source] " + state_id));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[terminal] " + terminal));
    AURORA_TEST_CHECK_THAT(text, m::has_substr(terminal + " --reads--> " + state_id));

    AURORA_TEST_CHECK_EQ(aurora::state_graph_text(), text);
    const auto document = aurora::state_graph();
    AURORA_TEST_CHECK_EQ(require_child(document, "nodes")->size(), StateGraph::nodes().size());
    AURORA_TEST_CHECK_EQ(require_child(document, "edges")->size(), StateGraph::edges().size());
}

}  // namespace aurora::test_cases::utest_state_graph
