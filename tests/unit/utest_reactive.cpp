/// 测试类型: unit
/// 目标单元: include/aurora/state/reactive.h
/// 测试说明: Reactive<T> 的默认/值/隐式转换/共享 State 构造、拷贝共享底层源、订阅委托与纯视图（read/空锚点）语义

#include <memory>
#include <string>
#include <utility>

#include "aurora/state/reactive.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_reactive {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）

AURORA_TEST_CASE(reactive_default_and_value_construction) {
    // 默认构造：值取 T{}。
    Reactive<int> empty{};
    AURORA_TEST_CHECK_EQ(empty.get(), 0);

    // 值构造：get() 返回当前值。
    Reactive<int> v{42};
    AURORA_TEST_CHECK_EQ(v.get(), 42);

    Reactive<std::string> s{std::string{"hi"}};
    AURORA_TEST_CHECK_THAT(s.get(), m::str_eq("hi"));
}

AURORA_TEST_CASE(reactive_implicit_conversion_from_convertible_values) {
    // 可转换为 T 的值隐式构造（属性字段 `content = "Hi"` 的关键路径，刻意不加 explicit）。
    Reactive<std::string> s = "hi";  // const char* → std::string
    AURORA_TEST_CHECK_THAT(s.get(), m::str_eq("hi"));

    Reactive<double> d = 3;  // int → double
    AURORA_TEST_CHECK_NEAR(d.get(), 3.0, 1e-9);
}

AURORA_TEST_CASE(reactive_shares_external_state_source) {
    // 由 shared_ptr<State<T>> 显式构造：多个 Reactive 与外部 State 共享同一信号源。
    auto source = std::make_shared<State<int>>(7);
    Reactive<int> a{source};
    Reactive<int> b{source};
    AURORA_TEST_CHECK_EQ(a.get(), 7);
    AURORA_TEST_CHECK_EQ(b.get(), 7);

    a.set(8);  // Reactive::set 写入底层 State
    AURORA_TEST_CHECK_EQ(source->get(), 8);
    AURORA_TEST_CHECK_EQ(b.get(), 8);

    source->set(9);  // 外部 State 更新对 Reactive 立即可见
    AURORA_TEST_CHECK_EQ(a.get(), 9);

    AURORA_TEST_CHECK(&a.state() == source.get());  // state() 暴露的正是共享源
}

AURORA_TEST_CASE(reactive_copy_shares_underlying_state) {
    // 拷贝 Reactive 只拷贝 shared_ptr：两份视图共享同一底层 State。
    Reactive<int> a{1};
    Reactive<int> b = a;
    AURORA_TEST_CHECK(&a.state() == &b.state());

    b.set(2);
    AURORA_TEST_CHECK_EQ(a.get(), 2);
    a.set(3);
    AURORA_TEST_CHECK_EQ(b.get(), 3);
}

AURORA_TEST_CASE(reactive_subscription_delegates_to_underlying_state) {
    // subscribe() 委托底层 State：手动订阅的 Effect 在 set 时重跑。
    Reactive<int> r{0};
    int runs = 0;
    Effect manual{[&] { ++runs; }};
    r.subscribe(manual);
    manual.run();  // 首跑（回调不读 r）
    AURORA_TEST_CHECK_EQ(runs, 1);
    r.set(1);
    AURORA_TEST_CHECK_EQ(runs, 2);

    // get() 在 Effect 作用域内读取同样经底层 State 自动登记依赖。
    Effect tracked{[&] {
        ++runs;
        (void)r.get();
    }};
    tracked.run();
    AURORA_TEST_CHECK_EQ(runs, 3);
    r.set(2);
    AURORA_TEST_CHECK_EQ(runs, 5);  // manual 与 tracked 均重跑
    AURORA_TEST_CHECK_EQ(r.get(), 2);
}

AURORA_TEST_CASE(reactive_view_semantics_null_anchor_and_read_dispatch) {
    // 纯信号视图语义：订阅总是委托内部 State，anchor() 默认返回空（signal_view.h 文档契约）。
    Reactive<int> r{1};
    AURORA_TEST_CHECK(r.anchor() == nullptr);
    SignalViewBase& base = r;
    AURORA_TEST_CHECK(base.anchor() == nullptr);

    // 经基类 read() 虚派发到 get()：在 Effect 作用域内读取即登记依赖。
    int runs = 0;
    Effect e{[&] {
        ++runs;
        base.read();
    }};
    e.run();
    AURORA_TEST_CHECK_EQ(runs, 1);
    r.set(4);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(r.get(), 4);
}

}  // namespace aurora::test_cases::utest_reactive
