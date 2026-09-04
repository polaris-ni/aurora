#pragma once

#include <vector>

#include "aurora/state/signal_view.h"

namespace aurora {

class StateBase;
class Effect;

namespace detail {

/// @brief 注册表条目：原始指针用于展示 / 读依赖边，弱引用锚点用于探测对象是否已析构。
///        遍历时跳过已失效条目（消除 append-only 注册表的悬垂解引用）。
struct StateRegEntry {
    StateBase *raw = nullptr;
    std::weak_ptr<ReactiveAnchor> anchor;
};
struct EffectRegEntry {
    Effect *raw = nullptr;
    std::weak_ptr<ReactiveAnchor> anchor;
};

/// @brief 响应式状态依赖图的运行期注册表（specification/02-state.md §6）。
///
/// State / Effect 在构造时登记自身，使 `StateGraph` 能枚举当前活着的节点并读出依赖边。
/// 注册表为 append-only（v1 不做注销）；条目携带弱引用锚点，遍历时跳过已析构对象，
/// 因此「陈旧条目」不再导致悬垂解引用。
inline auto registry_states() -> std::vector<StateRegEntry> & {
    static std::vector<StateRegEntry> v;
    return v;
}
inline auto registry_effects() -> std::vector<EffectRegEntry> & {
    static std::vector<EffectRegEntry> v;
    return v;
}

inline auto register_state(StateBase &s, const AnchorPtr &a) -> void {
    registry_states().push_back({.raw = &s, .anchor = a});
}
inline auto register_effect(Effect &e, const AnchorPtr &a) -> void {
    registry_effects().push_back({.raw = &e, .anchor = a});
}

}  // namespace detail

}  // namespace aurora
