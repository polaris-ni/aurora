#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_registry.h"
#include "aurora/widget/props_io.h"

namespace aurora {

/**
 * @brief 响应式状态依赖图（specification/02-state.md §6 状态作用域可追踪）。
 *
 * 从运行期活着的 State / Effect 网络读出节点（state / effect）与边
 * （state → effect 表示「观察」；effect → state 表示「依赖」），
 * 输出为 `Json` 或人类可读文本，供调试 / 文档 / 测试使用。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class StateGraph {
  public:
    struct Node {
        std::string id;
        std::string kind;  // "state" | "effect"
    };
    struct Edge {
        std::string from;
        std::string to;
        std::string kind;  // "observes" | "depends"
    };

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

    [[nodiscard]] static auto to_json() -> Json {
        Json j = Json::object();
        Json n = Json::array();
        for (const auto &[id, kind] : nodes()) {
            Json o = Json::object();
            o["id"] = id;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            o["kind"] = kind;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            n.push_back(o);
        }
        Json e = Json::array();
        for (const auto &[from, to, kind] : edges()) {
            Json o = Json::object();
            o["from"] = from;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            o["to"] = to;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            o["kind"] = kind;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
            e.push_back(o);
        }
        j["nodes"] = n;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        j["edges"] = e;  // NOLINT(*-pro-bounds-avoid-unchecked-container-access)
        return j;
    }

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

/// @brief 便捷自由函数（等价 `StateGraph::to_json` / `StateGraph::to_text`）。
[[nodiscard]] inline auto state_graph() -> Json { return StateGraph::to_json(); }
[[nodiscard]] inline auto state_graph_text() -> std::string { return StateGraph::to_text(); }

}  // namespace aurora
