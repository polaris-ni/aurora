#include "aurora/state/state_graph.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aurora/state/detail/reactor.h"
#include "aurora/state/detail/registry.h"

namespace aurora {
namespace {

/// @brief 来源节点的图 id：稳定身份加 `s` 前缀，与终端的编号空间区分。
/// @param node 来源节点。
/// @return 形如 `s12` 的稳定标识。
auto source_node_id(const detail::SourceNode &node) -> std::string { return "s" + std::to_string(node.id()); }

/// @brief 终端节点的图 id：登记序号加 `t` 前缀。
/// @param record 终端运行记录。
/// @return 形如 `t7` 的稳定标识。
auto terminal_node_id(const detail::RunRecord &record) -> std::string { return "t" + std::to_string(record.seq()); }

/// @brief 节点角色到图类别名的映射。
/// @param role 来源节点角色。
/// @return "source" 或 "derived"。
auto role_name(detail::SourceNode::NodeRole role) -> const char * {
    return role == detail::SourceNode::NodeRole::Derived ? "derived" : "source";
}

}  // namespace

auto StateGraph::nodes() -> std::vector<Node> {
    std::vector<Node> out;
    const auto sources = detail::live_sources();
    out.reserve(sources.size());
    for (const auto &source : sources) {
        out.push_back({.id = source_node_id(*source), .kind = role_name(source->role())});
    }
    for (const auto &record : detail::live_terminals()) {
        if (!record->cancelled()) {
            out.push_back({.id = terminal_node_id(*record), .kind = "terminal"});
        }
    }
    return out;
}

auto StateGraph::edges() -> std::vector<Edge> {
    std::vector<Edge> out;
    for (const auto &source : detail::live_sources()) {
        if (source->role() != detail::SourceNode::NodeRole::Derived) {
            continue;
        }
        for (const auto &dependency : source->dependency_nodes()) {
            if (dependency) {
                out.push_back({.from = source_node_id(*source), .to = source_node_id(*dependency), .kind = "reads"});
            }
        }
    }
    for (const auto &record : detail::live_terminals()) {
        if (record->cancelled()) {
            continue;
        }
        for (const auto &dependency : record->dependency_nodes()) {
            if (dependency) {
                out.push_back({.from = terminal_node_id(*record), .to = source_node_id(*dependency), .kind = "reads"});
            }
        }
    }
    return out;
}

auto StateGraph::to_json() -> json::Value {
    auto document = json::Value::object();
    document.set("schema", std::int64_t{1});
    auto node_array = json::Value::array();
    for (const auto &node : nodes()) {
        auto entry = json::Value::object();
        entry.set("id", node.id);
        entry.set("kind", node.kind);
        node_array.push_back(std::move(entry));
    }
    auto edge_array = json::Value::array();
    for (const auto &edge : edges()) {
        auto entry = json::Value::object();
        entry.set("from", edge.from);
        entry.set("to", edge.to);
        entry.set("kind", edge.kind);
        edge_array.push_back(std::move(entry));
    }
    document.set("nodes", std::move(node_array));
    document.set("edges", std::move(edge_array));
    return document;
}

auto StateGraph::to_text() -> std::string {
    std::ostringstream os;
    os << "StateGraph schema=1:\n";
    for (const auto &[id, kind] : nodes()) {
        os << "  [" << kind << "] " << id << "\n";
    }
    for (const auto &[from, to, kind] : edges()) {
        os << "  " << from << " --" << kind << "--> " << to << "\n";
    }
    return os.str();
}

}  // namespace aurora
