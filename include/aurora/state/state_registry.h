#pragma once

#include <vector>

#include "aurora/state/signal_view.h"

/// @brief Aurora 根命名空间：库的公共 API 均声明在其下。
namespace aurora {

class StateBase;
class Effect;

/// @brief 内部实现细节：跨 TU 共享的注册表设施，不属于公共 API 契约面。
namespace detail {

/// @brief 注册表条目：原始指针用于展示 / 读依赖边，弱引用锚点用于探测对象是否已析构。
///        遍历时跳过已失效条目（消除 append-only 注册表的悬垂解引用）。
struct StateRegEntry {
    StateBase *raw = nullptr;  ///< 被登记 State 的裸指针（仅展示/读边用，不拥有）
    std::weak_ptr<ReactiveAnchor> anchor;  ///< 对象存活探测弱锚：lock 失败 = State 已析构，遍历跳过
};
/// @brief Effect 侧注册表条目：与 StateRegEntry 同构，指向 Effect 实例。
struct EffectRegEntry {
    Effect *raw = nullptr;  ///< 被登记 Effect 的裸指针（仅展示/读边用，不拥有）
    std::weak_ptr<ReactiveAnchor> anchor;  ///< 对象存活探测弱锚：lock 失败 = Effect 已析构，遍历跳过
};

/// @brief 响应式状态依赖图的运行期注册表（specification/02-state.md §6）。
///
/// State / Effect 在构造时登记自身，使 `StateGraph` 能枚举当前活着的节点并读出依赖边。
/// 注册表为 append-only（v1 不做注销）；条目携带弱引用锚点，遍历时跳过已析构对象，
/// 因此「陈旧条目」不再导致悬垂解引用。
/// @return State 条目表的可变引用（进程级单例，首次调用惰性构造）。
inline auto registry_states() -> std::vector<StateRegEntry> & {
    static std::vector<StateRegEntry> v;
    return v;
}

/// @brief Effect 侧注册表（进程级单例存储，首次调用惰性构造）。
/// @return Effect 条目表的可变引用（append-only，条目存活由锚点探测）。
inline auto registry_effects() -> std::vector<EffectRegEntry> & {
    static std::vector<EffectRegEntry> v;
    return v;
}

/// @brief 登记一个 State 实例（StateBase 构造时调用；append-only，不注销）。
/// @param s 待登记的 State 实例（存其裸指针，不转移所有权）。
/// @param a 该实例的共享生命周期锚点，供遍历时探测是否已析构。
inline auto register_state(StateBase &s, const AnchorPtr &a) -> void {
    registry_states().push_back({.raw = &s, .anchor = a});
}

/// @brief 登记一个 Effect 实例（Effect 构造时调用；append-only，不注销）。
/// @param e 待登记的 Effect 实例（存其裸指针，不转移所有权）。
/// @param a 该实例的共享生命周期锚点，供遍历时探测是否已析构。
inline auto register_effect(Effect &e, const AnchorPtr &a) -> void {
    registry_effects().push_back({.raw = &e, .anchor = a});
}

}  // namespace detail

}  // namespace aurora
