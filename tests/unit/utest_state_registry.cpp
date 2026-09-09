/// 测试类型: unit
/// 目标单元: include/aurora/state/state_registry.h
/// 测试说明: 注册表条目结构、register_state/register_effect 的追加语义与锚点身份回传、State 构造自注册，以及析构后陈旧条目的过期探测与不查重追加

#include <cstddef>
#include <memory>

#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_registry.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_state_registry {

namespace detail = aurora::detail;

AURORA_TEST_CASE(register_state_appends_entry_with_raw_and_anchor) {
    // 手动注册：State 构造自注册 +1，手动再 +1；back 条目 raw 指向对象、锚点身份原样回传。
    auto& registry = detail::registry_states();
    const auto before = registry.size();

    State<int> s{1};
    const auto foreign = std::make_shared<aurora::ReactiveAnchor>();
    detail::register_state(s, foreign);

    AURORA_TEST_REQUIRE_EQ(registry.size(), before + 2);  // 自注册 + 手动
    const auto& entry = registry.back();
    AURORA_TEST_CHECK_EQ(entry.raw, static_cast<aurora::StateBase*>(&s));
    AURORA_TEST_CHECK_EQ(entry.anchor.lock(), foreign);
    AURORA_TEST_CHECK_FALSE(entry.anchor.expired());
}

AURORA_TEST_CASE(register_effect_appends_entry_with_raw_and_anchor) {
    // Effect 条目同构：构造自注册 +1，手动再 +1；back 条目锁定即得注册时传入的锚。
    auto& registry = detail::registry_effects();
    const auto before = registry.size();

    Effect eff{[] {}};
    const auto anchor = std::make_shared<aurora::ReactiveAnchor>();
    detail::register_effect(eff, anchor);

    AURORA_TEST_REQUIRE_EQ(registry.size(), before + 2);  // 自注册 + 手动
    const auto& entry = registry.back();
    AURORA_TEST_CHECK_EQ(entry.raw, &eff);
    AURORA_TEST_CHECK_EQ(entry.anchor.lock(), anchor);
    AURORA_TEST_CHECK_FALSE(entry.anchor.expired());
}

AURORA_TEST_CASE(state_construction_self_registers_with_live_anchor) {
    // StateBase 构造即自注册：条目锚点即 State 自带的 anchor_，对象存活期间可锁定。
    auto& registry = detail::registry_states();
    const auto before = registry.size();

    State<int> s{0};
    AURORA_TEST_REQUIRE_EQ(registry.size(), before + 1);
    const auto& entry = registry.back();
    AURORA_TEST_CHECK_EQ(entry.raw, static_cast<aurora::StateBase*>(&s));
    AURORA_TEST_CHECK_EQ(entry.anchor.lock(), s.anchor());
    AURORA_TEST_CHECK_FALSE(entry.anchor.expired());
}

AURORA_TEST_CASE(destroyed_state_leaves_stale_entry_with_expired_anchor) {
    // v1 不注销：对象析构后条目仍在，但弱锚点已过期——消费方（StateGraph）据此跳过陈旧条目。
    auto& registry = detail::registry_states();
    const auto before = registry.size();
    {
        State<int> doomed{0};
        AURORA_TEST_REQUIRE_EQ(registry.size(), before + 1);
    }
    AURORA_TEST_CHECK_EQ(registry.size(), before + 1);
    const auto& entry = registry[before];
    AURORA_TEST_CHECK_TRUE(entry.anchor.expired());
}

AURORA_TEST_CASE(repeated_registration_grows_without_dedup) {
    // 同一对象可重复注册：每调用一次恰好追加一条，互不覆盖（append-only 无查重）。
    auto& registry = detail::registry_states();
    const auto before = registry.size();

    State<int> s{0};
    const auto a1 = std::make_shared<aurora::ReactiveAnchor>();
    const auto a2 = std::make_shared<aurora::ReactiveAnchor>();
    detail::register_state(s, a1);
    detail::register_state(s, a2);

    AURORA_TEST_REQUIRE_EQ(registry.size(), before + 3);  // 自注册 + 2 次手动
    AURORA_TEST_CHECK_EQ(registry[before + 1].anchor.lock(), a1);
    AURORA_TEST_CHECK_EQ(registry[before + 2].anchor.lock(), a2);
    AURORA_TEST_CHECK_EQ(registry[before + 2].raw, static_cast<aurora::StateBase*>(&s));
}

}  // namespace aurora::test_cases::utest_state_registry
