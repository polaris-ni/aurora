/// 测试类型: unit
/// 目标单元: include/aurora/state/state_graph.h
/// 测试说明: StateGraph 对活 State 的节点与 observes 边枚举、Effect dispose 与析构后的陈旧条目过滤，以及 to_json/to_text 与自由函数等价输出

#include <sstream>
#include <string>
#include <vector>

#include "aurora/state/state_graph.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_state_graph {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）

/// @brief 复现 StateGraph::ptr_id 的指针标识（同为 os << const void*，平台格式一致），
///        用于把本用例创建的 State/Effect 与图节点/边按 id 关联。
[[nodiscard]] auto id_of(const void* p) -> std::string {
    std::ostringstream os;
    os << p;
    return os.str();
}

/// @brief State 节点 id 必须经 StateBase* 取得：State<T> 多继承（SignalView<T> 在前），
///        State<int>* 与注册表登记的 StateBase* 子对象地址数值不同，直接用 &s 会查不到节点/边。
[[nodiscard]] auto state_id_of(aurora::StateBase& s) -> std::string {
    return id_of(static_cast<const void*>(&s));
}

[[nodiscard]] auto find_node(const std::vector<StateGraph::Node>& nodes, const std::string& id)
    -> const StateGraph::Node* {
    for (const auto& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

[[nodiscard]] auto has_node(const std::vector<StateGraph::Node>& nodes, const std::string& id) -> bool {
    return find_node(nodes, id) != nullptr;
}

[[nodiscard]] auto has_edge(const std::vector<StateGraph::Edge>& edges, const std::string& from,
                            const std::string& to, const std::string& kind) -> bool {
    for (const auto& e : edges) {
        if (e.from == from && e.to == to && e.kind == kind) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto count_kind(const std::vector<StateGraph::Node>& nodes, const std::string& kind) -> std::size_t {
    std::size_t n = 0;
    for (const auto& node : nodes) {
        if (node.kind == kind) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] auto edge_references(const std::vector<StateGraph::Edge>& edges, const std::string& id) -> bool {
    for (const auto& e : edges) {
        if (e.from == id || e.to == id) {
            return true;
        }
    }
    return false;
}

AURORA_TEST_CASE(live_state_appears_as_node_with_observes_edge) {
    // 活 State：构造即入注册表（append-only，本用例内 +1）；Effect 运行期读取登记 observes 边。
    const auto states_before = count_kind(StateGraph::nodes(), "state");
    State<int> s{0};
    Effect eff{[&s] { (void)s.get(); }};
    eff.run();

    const auto nodes = StateGraph::nodes();
    const auto edges = StateGraph::edges();
    AURORA_TEST_CHECK_EQ(count_kind(nodes, "state"), states_before + 1);
    const auto* node = find_node(nodes, state_id_of(s));
    AURORA_TEST_REQUIRE_NOT_NULL(node);
    AURORA_TEST_CHECK_STREQ(node->kind, "state");
    AURORA_TEST_CHECK_TRUE(has_edge(edges, state_id_of(s), id_of(&eff), "observes"));
}

AURORA_TEST_CASE(disposed_effect_drops_observes_edge) {
    // dispose 后未析构：nodes()/edges() 须按 is_disposed 跳过，不再报告指向它的边。
    State<int> s{0};
    Effect eff{[&s] { (void)s.get(); }};
    eff.run();
    const auto effect_id = id_of(&eff);
    AURORA_TEST_REQUIRE_TRUE(has_edge(StateGraph::edges(), state_id_of(s), effect_id, "observes"));

    eff.dispose();
    for (const auto& e : StateGraph::edges()) {
        AURORA_TEST_CHECK_FALSE(e.to == effect_id);
    }
    AURORA_TEST_CHECK_NULL(find_node(StateGraph::nodes(), effect_id));
}

AURORA_TEST_CASE(stale_entries_are_skipped_after_destruction) {
    // 析构后注册表仍留陈旧条目（append-only），nodes()/edges() 必须按弱锚点过滤。
    std::string dead_state_id;
    std::string dead_effect_id;
    {
        State<int> dead{0};
        Effect eff{[&dead] { (void)dead.get(); }};
        eff.run();
        dead_state_id = state_id_of(dead);
        dead_effect_id = id_of(&eff);
        AURORA_TEST_REQUIRE_TRUE(has_node(StateGraph::nodes(), dead_state_id));
        AURORA_TEST_REQUIRE_TRUE(has_edge(StateGraph::edges(), dead_state_id, dead_effect_id, "observes"));
    }
    for (const auto& n : StateGraph::nodes()) {
        AURORA_TEST_CHECK_FALSE(n.id == dead_state_id);
    }
    AURORA_TEST_CHECK_FALSE(edge_references(StateGraph::edges(), dead_state_id));
    AURORA_TEST_CHECK_FALSE(edge_references(StateGraph::edges(), dead_effect_id));
}

AURORA_TEST_CASE(to_json_reports_nodes_and_edges_shape) {
    // JSON 形态：nodes/edges 数组，节点含 id+kind，边含 from+to+kind。
    State<int> s{0};
    Effect eff{[&s] { (void)s.get(); }};
    eff.run();
    const auto state_id = state_id_of(s);
    const auto effect_id = id_of(&eff);

    const auto j = aurora::state_graph();
    AURORA_TEST_REQUIRE_TRUE(j.contains("nodes"));
    AURORA_TEST_REQUIRE_TRUE(j["nodes"].is_array());
    AURORA_TEST_REQUIRE_TRUE(j.contains("edges"));
    AURORA_TEST_REQUIRE_TRUE(j["edges"].is_array());

    bool saw_state_node = false;
    for (const auto& item : j["nodes"]) {
        AURORA_TEST_REQUIRE(item.contains("id"));
        AURORA_TEST_REQUIRE(item.contains("kind"));
        if (item["id"].get<std::string>() == state_id) {
            saw_state_node = true;
            AURORA_TEST_CHECK_EQ(item["kind"].get<std::string>(), std::string("state"));
        }
    }
    AURORA_TEST_CHECK_TRUE(saw_state_node);

    bool saw_observes = false;
    for (const auto& item : j["edges"]) {
        AURORA_TEST_REQUIRE(item.contains("from"));
        AURORA_TEST_REQUIRE(item.contains("to"));
        AURORA_TEST_REQUIRE(item.contains("kind"));
        if (item["kind"].get<std::string>() == "observes" && item["from"].get<std::string>() == state_id &&
            item["to"].get<std::string>() == effect_id) {
            saw_observes = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(saw_observes);
}

AURORA_TEST_CASE(to_text_lists_nodes_edges_and_matches_free_function) {
    // 文本形态以 "StateGraph:" 起头，逐行列出节点与 "--kind-->" 边；自由函数与静态成员等价。
    State<int> s{0};
    Effect eff{[&s] { (void)s.get(); }};
    eff.run();

    const auto text = StateGraph::to_text();
    AURORA_TEST_CHECK_THAT(text, m::starts_with("StateGraph:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[state]"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr(state_id_of(s)));  // state 节点 id 用 StateBase* 子对象地址
    AURORA_TEST_CHECK_THAT(text, m::has_substr("--observes-->"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr(id_of(&eff)));
    // observes 边端点校验：from=StateBase 子对象地址 → to=Effect 地址。
    AURORA_TEST_CHECK_THAT(text, m::has_substr(state_id_of(s) + " --observes--> " + id_of(&eff)));

    AURORA_TEST_CHECK_EQ(aurora::state_graph_text(), text);
    const auto j = aurora::state_graph();
    AURORA_TEST_CHECK_EQ(j["nodes"].size(), StateGraph::nodes().size());
    AURORA_TEST_CHECK_EQ(j["edges"].size(), StateGraph::edges().size());
}

}  // namespace aurora::test_cases::utest_state_graph
