/// 测试类型: unit
/// 目标单元: include/aurora/state/binding.h
/// 测试说明: Binding<T> 的构造与不变量（bound/target/removable）、对上游 State
/// 的读写与观察者通知、可选删除回调的每次触发，以及未绑定访问的常开死亡路径

#include "aurora/state/binding.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_binding {

AURORA_TEST_CASE(default_binding_is_unbound_and_remove_is_noop) {
    // 默认构造：未绑定任何上游；无删除回调时 remove() 是安全空操作。
    Binding<int> b;
    AURORA_TEST_CHECK_FALSE(b.bound());
    AURORA_TEST_CHECK_NULL(b.target());
    AURORA_TEST_CHECK_FALSE(b.removable());
    AURORA_TEST_CHECK_NO_THROW(b.remove());
}

AURORA_TEST_CASE(binding_reads_and_writes_bound_state) {
    // 绑定到普通 State：get/set 即上游 State 的读写视图（non-owning，target() 暴露上游指针）。
    State<int> s{5};
    Binding<int> b{s};
    AURORA_TEST_CHECK_TRUE(b.bound());
    AURORA_TEST_CHECK_EQ(b.target(), &s);
    AURORA_TEST_CHECK_EQ(b.get(), 5);

    b.set(7);  // 经 Binding 写穿到上游 State
    AURORA_TEST_CHECK_EQ(s.get(), 7);
    AURORA_TEST_CHECK_EQ(b.get(), 7);

    s.set(9);  // 上游自行写入同样经 Binding 可见
    AURORA_TEST_CHECK_EQ(b.get(), 9);
}

AURORA_TEST_CASE(binding_set_notifies_state_observers) {
    // Binding::set → State::set → notify：依赖该 State 的 Effect 重跑（定点刷新语义）。
    State<int> s{0};
    int runs = 0;
    int last = -1;
    Effect eff{[&]() -> void {
        last = s.get();
        ++runs;
    }};
    eff.run();  // 首次应用当前值并登记依赖
    AURORA_TEST_REQUIRE_EQ(last, 0);

    Binding<int> b{s};
    b.set(3);
    AURORA_TEST_CHECK_EQ(runs, 2);
    AURORA_TEST_CHECK_EQ(last, 3);
}

AURORA_TEST_CASE(unbound_get_and_set_are_fatal) {
    // 已知契约：get/set 在未绑定时 AURORA_CHECK 常开拦截（空解引用 = UB），进程异常终止。
    Binding<int> b;
    AURORA_TEST_CHECK_DEATH((void)b.get(), "Binding accessed before bound");
    AURORA_TEST_CHECK_DEATH(b.set(1), "Binding accessed before bound");
}

AURORA_TEST_CASE(remove_invokes_injected_remover_each_call) {
    // 注入删除回调后 removable() 为真；remove() 每次调用都触发回调（非幂等），
    // 语义对应 Preferences::binding 的持久化键删除（多进程墓碑由存储层完成）。
    State<int> s{1};
    int removed = 0;
    Binding<int> b{s, [&]() -> void { ++removed; }};
    AURORA_TEST_CHECK_TRUE(b.bound());
    AURORA_TEST_CHECK_TRUE(b.removable());
    AURORA_TEST_CHECK_EQ(b.get(), 1);  // 删除回调的存在不影响读写

    b.remove();
    AURORA_TEST_CHECK_EQ(removed, 1);
    b.remove();
    AURORA_TEST_CHECK_EQ(removed, 2);
}

}  // namespace aurora::test_cases::utest_binding
