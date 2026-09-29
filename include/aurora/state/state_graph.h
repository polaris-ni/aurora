#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "aurora/core/json.h"
#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_registry.h"

namespace aurora {

/// @brief 响应式状态依赖图（specification/02-state.md §6 状态作用域可追踪）。
///
/// 从运行期活着的 State / Effect 网络读出节点（state / effect）与边
/// （state → effect 表示「观察」；effect → state 表示「依赖」），
/// 输出为 `json::Value` 或人类可读文本，供调试 / 文档 / 测试使用。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class StateGraph {
  public:
    /// @brief 图节点：一个存活 State 或 Effect 的标识。
    struct Node {
        std::string id;  ///< 节点标识（对象地址的字符串形式）
        std::string kind;  ///< 节点类别："state" | "effect"
    };
    /// @brief 图边：State 与 Effect 之间的有向观察/依赖关系。
    struct Edge {
        std::string from;  ///< 起点节点 id（地址字符串）
        std::string to;  ///< 终点节点 id（地址字符串）
        std::string kind;  ///< 边类别："observes"（state→effect）| "depends"（effect→state）
    };

    /// @brief 枚举运行期存活的 State / Effect 节点（经全局注册表 + 锚点探活）。
    /// @return 存活节点列表；锚点已失效或已 dispose 的陈旧条目被跳过。
    [[nodiscard]] static auto nodes() -> std::vector<Node> {
        std::vector<Node> out;
        for (const auto &[raw, anchor] : detail::registry_states()) {
            if (!anchor.lock()) {
                continue;  // 已析构，跳过陈旧条目
            }
            if (raw != nullptr) {
                out.push_back({.id = ptr_id(raw), .kind = "state"});
            }
        }
        for (const auto &ent : detail::registry_effects()) {
            if (!ent.anchor.lock()) {
                continue;
            }
            const Effect *e = ent.raw;
            if (e != nullptr && !e->is_disposed()) {
                out.push_back({.id = ptr_id(e), .kind = "effect"});
            }
        }
        return out;
    }

    /// @brief 枚举存活观察边（state→effect）与依赖边（effect→state）。
    /// @return 边列表；两端锚点失效或已 dispose 的边被跳过，绝不解引用悬垂对象。
    [[nodiscard]] static auto edges() -> std::vector<Edge> {
        std::vector<Edge> out;
        for (const auto &ent : detail::registry_states()) {
            if (!ent.anchor.lock()) {
                continue;  // 已析构，跳过陈旧条目
            }
            const StateBase *s = ent.raw;
            if (s == nullptr) {
                continue;
            }
            for (const auto &c : s->observers_) {
                // 仅当 Effect 锚点存活（effect 锁定成功）时才读取 effect_raw，
                // 否则为失效边，直接跳过（避免解引用已析构 Effect）。
                if (!c->effect.lock()) {
                    continue;
                }
                const Effect *e = c->effect_raw;
                if (e->is_disposed()) {
                    continue;
                }
                out.push_back({.from = ptr_id(s), .to = ptr_id(e), .kind = "observes"});
            }
        }
        for (const auto &[raw, anchor] : detail::registry_effects()) {
            if (!anchor.lock()) {
                continue;
            }
            const Effect *e = raw;
            if ((e == nullptr) || e->is_disposed()) {
                continue;
            }
            for (const auto &[dep_raw, dep_anchor] : e->deps_) {
                if (dep_anchor.lock()) {
                    out.push_back({.from = ptr_id(e), .to = ptr_id(dep_raw), .kind = "depends"});
                }
            }
        }
        return out;
    }

    /// @brief 将当前状态依赖图序列化为 JSON（`{nodes:[{id,kind}], edges:[{from,to,kind}]}`）。
    /// @return 含 nodes/edges 两个数组的 `json::Value` 对象，供调试面板或测试断言消费。
    [[nodiscard]] static auto to_json() -> json::Value {
        auto j = json::Value::object();
        auto n = json::Value::array();
        for (const auto &[id, kind] : nodes()) {
            auto o = json::Value::object();
            o.set("id", id);
            o.set("kind", kind);
            n.push_back(o);
        }
        auto e = json::Value::array();
        for (const auto &[from, to, kind] : edges()) {
            auto o = json::Value::object();
            o.set("from", from);
            o.set("to", to);
            o.set("kind", kind);
            e.push_back(o);
        }
        j.set("nodes", n);
        j.set("edges", e);
        return j;
    }

    /// @brief 将当前状态依赖图渲染为人类可读的多行文本（节点段 + 边段）。
    /// @return 文本快照，每行一个节点或 `from --kind--> to` 形式的边。
    [[nodiscard]] static auto to_text() -> std::string {
        std::ostringstream os;
        os << "StateGraph:\n";
        for (const auto &[id, kind] : nodes()) {
            os << "  [" << kind << "] " << id << "\n";
        }
        for (const auto &[from, to, kind] : edges()) {
            os << "  " << from << " --" << kind << "--> " << to << "\n";
        }
        return os.str();
    }

  private:
    static auto ptr_id(const void *p) -> std::string {
        std::ostringstream os;
        os << p;
        return os.str();
    }

    friend class StateBase;
    friend class Effect;
};

/// @brief 便捷自由函数：返回当前状态依赖图的 JSON 快照（等价 `StateGraph::to_json()`）。
/// @return 含 nodes/edges 两数组的 `json::Value` 对象。
[[nodiscard]] inline auto state_graph() -> json::Value { return StateGraph::to_json(); }

/// @brief 便捷自由函数：返回当前状态依赖图的人类可读文本（等价 `StateGraph::to_text()`）。
/// @return 多行文本快照，供日志与调试输出。
[[nodiscard]] inline auto state_graph_text() -> std::string { return StateGraph::to_text(); }

}  // namespace aurora
