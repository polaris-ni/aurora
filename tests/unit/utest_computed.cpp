/// 测试类型: unit
/// 目标单元: include/aurora/state/computed.h
/// 测试说明: Computed<T> 构造求值与缓存、依赖变化重算、Effect 通知、链式派生、条件依赖分支、异常传播与 computed 工厂类型推导

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "aurora/state/computed.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_computed {

AURORA_TEST_CASE(computed_construction_evaluates_and_caches) {
    // 构造即求值；get() 返回缓存值，重复读取不重算。
    State<int> n{4};
    int calls = 0;
    Computed<int> c{[&] {
        ++calls;
        return n.get() * n.get();
    }};
    const int calls_after_ctor = calls;
    AURORA_TEST_CHECK(calls_after_ctor >= 1);  // 构造至少求值一次
    AURORA_TEST_CHECK_EQ(c.get(), 16);
    AURORA_TEST_CHECK_EQ(c.get(), 16);
    AURORA_TEST_CHECK_EQ(calls, calls_after_ctor);  // get() 走缓存
}

AURORA_TEST_CASE(computed_recomputes_on_dependency_change) {
    // 依赖 State 变化 → 内部 Effect 重跑 → 缓存值更新。
    State<int> n{1};
    Computed<int> c{[&] { return n.get() + 1; }};
    AURORA_TEST_CHECK_EQ(c.get(), 2);
    n.set(5);
    AURORA_TEST_CHECK_EQ(c.get(), 6);
    n.set(5);  // 同值写入仍通知（State 契约），重算结果不变
    AURORA_TEST_CHECK_EQ(c.get(), 6);
}

AURORA_TEST_CASE(computed_notifies_dependent_effect) {
    // Computed::get() 在 Effect 作用域内登记自身为依赖；重算后通知观察者。
    State<int> a{1};
    Computed<int> c{[&] { return a.get() * 2; }};
    int runs = 0;
    int last = 0;
    Effect e{[&] {
        ++runs;
        last = c.get();
    }};
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 1);
    AURORA_TEST_CHECK_EQ(last, 2);
    a.set(3);
    AURORA_TEST_CHECK_EQ(runs, 2);  // a.set → Computed 重算 → 通知 e 重跑
    AURORA_TEST_CHECK_EQ(last, 6);
    AURORA_TEST_CHECK_EQ(c.get(), 6);
}

AURORA_TEST_CASE(computed_chained_derivation_propagates) {
    // 链式派生：上游 Computed 变化传播到下游 Computed。
    State<int> base{1};
    auto up = computed([&] { return base.get() + 1; });
    Computed<int> down{[&] { return up.get() * 10; }};
    AURORA_TEST_CHECK_EQ(up.get(), 2);
    AURORA_TEST_CHECK_EQ(down.get(), 20);
    base.set(2);
    AURORA_TEST_CHECK_EQ(up.get(), 3);
    AURORA_TEST_CHECK_EQ(down.get(), 30);
}

AURORA_TEST_CASE(computed_conditional_reads_follow_current_branch) {
    // 条件读取：只有本轮实际读取的分支依赖影响缓存值（按值断言，边建立语义见 effect 测试）。
    State<int> mode{0};
    State<int> x{1};
    State<int> y{2};
    Computed<int> c{[&] { return mode.get() != 0 ? x.get() : y.get(); }};
    AURORA_TEST_CHECK_EQ(c.get(), 2);  // y 分支
    x.set(10);                         // x 未被读取 → 缓存值不变
    AURORA_TEST_CHECK_EQ(c.get(), 2);
    mode.set(1);                       // 切换分支 → 重算读取 x
    AURORA_TEST_CHECK_EQ(c.get(), 10);
    y.set(99);
    AURORA_TEST_CHECK_EQ(c.get(), 10);  // 本轮值仍取 x
    x.set(20);
    AURORA_TEST_CHECK_EQ(c.get(), 20);
}

AURORA_TEST_CASE(computed_throwing_fn_propagates_from_construction) {
    // 错误路径：fn 在构造期抛出 → 异常直接传播。
    // 注：此处刻意只测构造期抛出（发生在任何 Effect run 之前）；运行期（依赖更新重算）抛出
    // 会使 Effect::current_ 失去恢复机会而污染后续所有用例，详见返回报告，不纳入用例。
    State<int> hot{1};
    AURORA_TEST_CHECK_THROW(
        Computed<int>{[&]() -> int {
            if (hot.get() > 0) {
                throw std::runtime_error("boom at construction");
            }
            return hot.get();
        }},
        std::runtime_error);

    // 构造中止是干净的：源状态未产生观察边，活跃作用域未被污染。
    AURORA_TEST_CHECK_EQ(hot.get(), 1);
    AURORA_TEST_CHECK(Effect::current() == nullptr);

    // 同一 State 仍可正常派生。
    Computed<int> c{[&] { return hot.get() * 2; }};
    AURORA_TEST_CHECK_EQ(c.get(), 2);
    hot.set(3);
    AURORA_TEST_CHECK_EQ(c.get(), 6);
}

AURORA_TEST_CASE(computed_factory_deduces_value_type) {
    // 工厂 computed(F) 从返回类型推导 T；remove_cvref：返回 const 引用也推导为值类型。
    State<int> n{3};
    auto doubled = computed([&] { return n.get() * 2; });
    static_assert(std::is_same_v<decltype(doubled), Computed<int>>);
    AURORA_TEST_CHECK_EQ(doubled.get(), 6);
    n.set(5);
    AURORA_TEST_CHECK_EQ(doubled.get(), 10);

    State<std::string> word{std::string{"a"}};
    auto shout = computed([&] { return word.get() + "!"; });
    static_assert(std::is_same_v<decltype(shout), Computed<std::string>>);
    AURORA_TEST_CHECK_EQ(shout.get(), std::string{"a!"});

    auto aliased = computed([&]() -> const int& { return n.get(); });
    static_assert(std::is_same_v<decltype(aliased), Computed<int>>);
    AURORA_TEST_CHECK_EQ(aliased.get(), 5);
}

}  // namespace aurora::test_cases::utest_computed
