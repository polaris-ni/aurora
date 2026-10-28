#pragma once

#include <string>
#include <vector>

#include "aurora/core/json.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

/// @brief 响应式状态依赖图（specification/02-state.md §6 状态作用域可追踪）。
///
/// 从运行期活着的来源（`State` / `ReadOnlyState` / `computed`）与终端运行记录
/// （订阅 / `effect` / 控件信号接线）读出节点与边：
/// - 节点 `kind`：`source`（值来源）、`derived`（派生来源）、`terminal`（终端运行记录）；
/// - 边 `from --reads--> to`：观察者（派生或终端）读取来源。
///
/// 节点 id 取引擎分配的稳定身份（`s<编号>` / `t<编号>`），不随对象地址变化；
/// 复制句柄不新增节点，已析构节点与其边自动从快照中消失。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class StateGraph {
  public:
    /// @brief 图节点：一个存活来源或终端的标识。
    struct Node {
        std::string id;  ///< 节点标识（稳定身份，`s<编号>` 或 `t<编号>`）
        std::string kind;  ///< 节点类别："source" | "derived" | "terminal"
    };
    /// @brief 图边：观察者到其读取来源的有向边。
    struct Edge {
        std::string from;  ///< 观察者节点 id（派生或终端）
        std::string to;  ///< 被读取来源节点 id
        std::string kind;  ///< 边类别：恒为 "reads"
    };

    /// @brief 枚举运行期存活的节点。
    /// @return 节点列表：来源按登记顺序在前，终端按登记顺序在后；已析构节点被剔除。
    [[nodiscard]] static auto nodes() -> std::vector<Node>;

    /// @brief 枚举存活读取边（派生/终端 → 其依赖来源）。
    /// @return 边列表；两端任一已失效的边不会出现。
    [[nodiscard]] static auto edges() -> std::vector<Edge>;

    /// @brief 将当前状态依赖图序列化为 JSON（`{schema, nodes:[{id,kind}], edges:[{from,to,kind}]}`）。
    /// @return 含 schema（当前为 1）、nodes、edges 三键的 `json::Value` 对象。
    [[nodiscard]] static auto to_json() -> json::Value;

    /// @brief 将当前状态依赖图渲染为人类可读的多行文本（首行为 `StateGraph schema=1:`）。
    /// @return 文本快照，每行一个节点或 `from --reads--> to` 形式的边。
    [[nodiscard]] static auto to_text() -> std::string;
};

/// @brief 便捷自由函数：返回当前状态依赖图的 JSON 快照（等价 `StateGraph::to_json()`）。
/// @return 含 schema/nodes/edges 三键的 `json::Value` 对象。
[[nodiscard]] inline auto state_graph() -> json::Value { return StateGraph::to_json(); }

/// @brief 便捷自由函数：返回当前状态依赖图的人类可读文本（等价 `StateGraph::to_text()`）。
/// @return 多行文本快照，供日志与调试输出。
[[nodiscard]] inline auto state_graph_text() -> std::string { return StateGraph::to_text(); }

}  // namespace aurora
