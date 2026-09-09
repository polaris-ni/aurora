/// 测试类型: unit
/// 目标单元: include/aurora/state/state.h
/// 测试说明: State<T> 构造与值语义、get 作用域内登记依赖与去重、const 读取、dispose/析构观察边安全、shared() 与移动-only 值类型

#include <memory>
#include <string>
#include <utility>

#include "aurora/state/state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_state {

AURORA_TEST_CASE(state_default_and_value_construction) {
    // 默认构造：值取 T{}；显式值构造：get() 返回初值。
    State<int> s{};
    State<int> t{3};
    AURORA_TEST_CHECK_EQ(s.get(), 0);
    AURORA_TEST_CHECK_EQ(t.get(), 3);

    // 每个实例持有独立且非空的生命周期锚点。
    AURORA_TEST_CHECK(s.anchor() != nullptr);
    AURORA_TEST_CHECK(t.anchor() != nullptr);
    AURORA_TEST_CHECK(s.anchor() != t.anchor());

    // get() 返回内部存储的引用：set 后旧引用观察到新值。
    State<std::string> str{std::string{"a"}};
    const std::string& alias = str.get();
    str.set(std::string{"b"});
    AURORA_TEST_CHECK_EQ(alias, std::string{"b"});
}

AURORA_TEST_CASE(state_set_notifies_subscribed_effect) {
    // subscribe() 手动建立观察边；set() 通知观察者重跑（定点刷新）。
    State<int> s{0};
    int runs = 0;
    Effect e{[&] { ++runs; }};
    s.subscribe(e);
    e.run();  // 首跑（回调不读 s）
    AURORA_TEST_CHECK_EQ(runs, 1);
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(s.get(), 1);
}

AURORA_TEST_CASE(state_get_outside_effect_scope_does_not_subscribe) {
    // 作用域外 get() 只取值，不登记依赖：未订阅的 Effect 不因 set 重跑。
    State<int> s{0};
    int runs = 0;
    Effect e{[&] { ++runs; }};
    e.run();  // 回调不读 s
    (void)s.get();
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(s.get(), 1);
}

AURORA_TEST_CASE(state_get_in_effect_scope_registers_and_dedups) {
    // 作用域内 get() 自动登记依赖；同一 Effect 重复登记被去重（动画每帧重跑不累积观察边）。
    State<int> s{0};
    int runs = 0;
    Effect e{[&] {
        ++runs;
        (void)s.get();
    }};
    e.run();
    e.run();  // 二次登记同一 Effect
    AURORA_TEST_CHECK_EQ(runs, 2);
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs, 3);  // 一次 set 恰好触发一次重跑（若边重复会是 4）
    AURORA_TEST_CHECK_EQ(s.get(), 1);
}

AURORA_TEST_CASE(state_const_get_still_registers_dependency) {
    // 经 const State& 读取同样登记依赖（get() const 内部去 const 订阅，接口约束使然）。
    auto owned = std::make_shared<State<int>>(5);
    const State<int>& view = *owned;
    int runs = 0;
    Effect e{[&] {
        ++runs;
        (void)view.get();
    }};
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 1);
    owned->set(6);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(view.get(), 6);
}

AURORA_TEST_CASE(state_disposed_and_destroyed_effects_are_skipped_safely) {
    // 已 dispose 的 Effect 被跳过；已析构 Effect 的失效边在 notify 时惰性摘除，不悬垂。
    State<int> s{0};
    int runs_a = 0;
    int runs_b = 0;
    Effect disposed_e{[&] {
        ++runs_a;
        (void)s.get();
    }};
    disposed_e.run();
    AURORA_TEST_CHECK_EQ(runs_a, 1);
    disposed_e.dispose();
    {
        Effect transient{[&] {
            ++runs_b;
            (void)s.get();
        }};
        transient.run();
        AURORA_TEST_CHECK_EQ(runs_b, 1);
    }  // transient 析构 → 锚点释放
    s.set(1);
    AURORA_TEST_CHECK_EQ(runs_a, 1);  // dispose 后不再重跑
    AURORA_TEST_CHECK_EQ(runs_b, 1);  // 析构边被安全摘除
    AURORA_TEST_CHECK_EQ(s.get(), 1);
    disposed_e.run();  // dispose 后 run() 为空操作
    AURORA_TEST_CHECK_EQ(runs_a, 1);
}

AURORA_TEST_CASE(state_shared_returns_managed_self) {
    // shared() 返回管理同一对象的 shared_ptr（状态提升给子组件的惯用路径）。
    auto s = std::make_shared<State<int>>(5);
    auto same = s->shared();
    AURORA_TEST_CHECK(same.get() == s.get());
    AURORA_TEST_CHECK_GE(same.use_count(), 2);
    same->set(9);
    AURORA_TEST_CHECK_EQ(s->get(), 9);
    s->set(10);
    AURORA_TEST_CHECK_EQ(same->get(), 10);
}

AURORA_TEST_CASE(state_move_only_value_type_supported) {
    // 移动-only 值类型可用：默认构造为 nullptr，set 以移动写入。
    State<std::unique_ptr<int>> empty{};
    AURORA_TEST_CHECK(empty.get() == nullptr);

    State<std::unique_ptr<int>> s{std::unique_ptr<int>{}};
    AURORA_TEST_CHECK(s.get() == nullptr);
    s.set(std::make_unique<int>(7));
    AURORA_TEST_CHECK(s.get() != nullptr);
    AURORA_TEST_CHECK_EQ(*s.get(), 7);
}

}  // namespace aurora::test_cases::utest_state
