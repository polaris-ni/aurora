/// 测试类型: unit
/// 目标单元: include/aurora/state/effect.h
/// 测试说明: Effect 构造不执行、current 作用域嵌套与恢复、依赖重跑与观察边叠加语义、dispose 停跑、非拷贝/移动与锚点、析构后源可继续写

#include <functional>
#include <type_traits>

#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_effect {

AURORA_TEST_CASE(effect_constructor_defers_first_run) {
    // 构造函数不执行 fn（区别于 Computed）；须显式 run() 首跑。
    int runs = 0;
    Effect e{[&] { ++runs; }};
    AURORA_TEST_CHECK_EQ(runs, 0);
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 1);

    // 空 std::function 的 Effect：run() 为安全空操作。
    Effect empty{std::function<void()>{}};
    AURORA_TEST_CHECK_NO_THROW(empty.run());
}

AURORA_TEST_CASE(effect_current_tracks_active_scope_with_nested_restore) {
    // current()：作用域外为空；run 期间指向该 Effect；嵌套 run 内层替换、结束恢复外层、最外层结束恢复空。
    AURORA_TEST_CHECK(Effect::current() == nullptr);
    Effect* seen_outer = nullptr;
    Effect* seen_inner = nullptr;
    Effect outer{[&] {
        seen_outer = Effect::current();
        Effect inner{[&] { seen_inner = Effect::current(); }};
        inner.run();
    }};
    outer.run();
    AURORA_TEST_CHECK(seen_outer == &outer);
    AURORA_TEST_CHECK(seen_inner != nullptr);
    AURORA_TEST_CHECK(seen_inner != &outer);  // 内层作用域替换了外层
    AURORA_TEST_CHECK(Effect::current() == nullptr);
}

AURORA_TEST_CASE(effect_reruns_only_for_signals_read_in_scope) {
    // 只有 run 期间读取过的信号在 set 时触发重跑；从未读取的信号不影响本 Effect。
    State<bool> flag{false};
    State<int> x{10};
    int runs = 0;
    Effect e{[&] {
        ++runs;
        (void)(flag.get() ? x.get() : 0);
    }};
    e.run();  // 首跑：读 flag（x 未读）
    AURORA_TEST_CHECK_EQ(runs, 1);
    x.set(11);  // x 从未被读取 → 不重跑
    AURORA_TEST_CHECK_EQ(runs, 1);
    flag.set(true);  // 依赖变化 → 重跑，本轮起读取 x
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(x.get(), 11);
}

AURORA_TEST_CASE(effect_previous_reads_keep_triggering_reruns) {
    // 观察边是叠加的：历史轮读取过的信号在其后 set 时仍触发重跑（State 侧不做边剪枝）。
    State<int> a{0};
    State<int> b{0};
    int runs = 0;
    Effect e{[&] {
        ++runs;
        (void)(a.get() + (runs > 1 ? b.get() : 0));
    }};
    e.run();  // 首跑只读 a
    AURORA_TEST_CHECK_EQ(runs, 1);
    b.set(1);  // b 未被任何轮读取 → 不重跑
    AURORA_TEST_CHECK_EQ(runs, 1);
    a.set(2);  // a 的观察边触发重跑；本轮起读取 b
    AURORA_TEST_CHECK_EQ(runs, 2);
    b.set(3);  // b 已在上一轮读取 → 观察边仍在，仍触发重跑
    AURORA_TEST_CHECK_EQ(runs, 3);
}

AURORA_TEST_CASE(effect_dispose_stops_execution) {
    // dispose() 后：is_disposed 置位、State set 不再触发重跑、run() 为空操作。
    State<int> s{0};
    int runs = 0;
    Effect e{[&] {
        ++runs;
        (void)s.get();
    }};
    e.run();
    AURORA_TEST_CHECK_FALSE(e.is_disposed());
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 2);
    e.dispose();
    AURORA_TEST_CHECK_TRUE(e.is_disposed());
    s.set(2);
    AURORA_TEST_CHECK_EQ(runs, 2);
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(s.get(), 2);
}

AURORA_TEST_CASE(effect_non_copyable_non_movable_with_per_instance_anchor) {
    // Effect 不可拷贝、不可移动；每实例持有独立非空锚点。
    static_assert(!std::is_copy_constructible_v<Effect>);
    static_assert(!std::is_copy_assignable_v<Effect>);
    static_assert(!std::is_move_constructible_v<Effect>);
    static_assert(!std::is_move_assignable_v<Effect>);
    Effect e{[] {}};
    Effect f{[] {}};
    AURORA_TEST_CHECK(e.anchor() != nullptr);
    AURORA_TEST_CHECK(f.anchor() != nullptr);
    AURORA_TEST_CHECK(e.anchor() != f.anchor());
}

AURORA_TEST_CASE(effect_destroyed_observer_keeps_state_settable) {
    // Effect 析构后（锚点释放），State 继续 set 安全：失效边由 notify 惰性摘除，绝不悬垂。
    State<int> s{0};
    {
        Effect e{[&] { (void)s.get(); }};
        e.run();
    }
    AURORA_TEST_CHECK_NO_THROW(s.set(1));
    AURORA_TEST_CHECK_EQ(s.get(), 1);
    AURORA_TEST_CHECK_NO_THROW(s.set(2));  // 摘除后再次 set 仍安全
    AURORA_TEST_CHECK_EQ(s.get(), 2);
}

}  // namespace aurora::test_cases::utest_effect
