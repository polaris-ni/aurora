/// 测试类型: unit
/// 目标单元: include/aurora/state/state_registry.h
/// 测试说明: 响应式运行期注册表（State 构造自登记、条目弱锚点随析构失效、append-only、register_effect 显式登记）单元测试

#include <memory>

#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_registry.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_state_registry {

AURORA_TEST() {
    // ---- 1. State 构造即在 registry_states() 追加一条自登记条目 ----
    const auto baseline = detail::registry_states().size();
    {
        State<int> s{1};
        AURORA_TEST_CHECK_EQ(detail::registry_states().size(), baseline + 1);

        const auto &ent = detail::registry_states().back();
        AURORA_TEST_CHECK(ent.raw != nullptr);
        AURORA_TEST_CHECK(!ent.anchor.expired());           // 存活期：弱锚点可锁定
        AURORA_TEST_CHECK(ent.anchor.lock() == s.anchor()); // 与 State 的锚点同一控制块
    }

    // ---- 2. append-only：State 析构后条目仍在，但弱锚点已失效 ----
    {
        AURORA_TEST_CHECK_EQ(detail::registry_states().size(), baseline + 1);  // 未被移除
        const auto &ent = detail::registry_states().back();
        AURORA_TEST_CHECK(ent.anchor.expired());  // 对象已析构 → 探测失效，遍历时可跳过
    }

    // ---- 3. 多个 State 各自登记，顺序稳定 ----
    {
        const auto before = detail::registry_states().size();
        State<double> a{1.0};
        State<double> b{2.0};
        AURORA_TEST_CHECK_EQ(detail::registry_states().size(), before + 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(detail::registry_states()[before].anchor.lock() == a.anchor());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(detail::registry_states()[before + 1].anchor.lock() == b.anchor());
    }

    // ---- 4. register_effect：显式登记 Effect 条目（raw 指向 Effect、锚点随其存活） ----
    {
        const auto before = detail::registry_effects().size();
        Effect e([]() -> void {});
        detail::register_effect(e, e.anchor());
        AURORA_TEST_CHECK_EQ(detail::registry_effects().size(), before + 1);
        const auto &ent = detail::registry_effects().back();
        AURORA_TEST_CHECK(ent.raw == &e);
        AURORA_TEST_CHECK(ent.anchor.lock() == e.anchor());
    }
}

}  // namespace aurora::test_cases::utest_state_registry
