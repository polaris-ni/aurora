/// 测试类型: unit
/// 目标单元: include/aurora/state/binding.h
/// 测试说明: 双向绑定引用（get/set 透传上游 State、bound/target 探测、可选删除回调 removable/remove）单元测试

#include "aurora/state/binding.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_binding {

AURORA_TEST() {
    // ---- 1. 默认构造：未绑定，target 为空、不可删除 ----
    {
        const Binding<int> unbound;
        AURORA_TEST_CHECK_FALSE(unbound.bound());
        AURORA_TEST_CHECK(unbound.target() == nullptr);
        AURORA_TEST_CHECK_FALSE(unbound.removable());
        unbound.remove();  // 无回调时为安全空操作，不得崩溃
        AURORA_TEST_CHECK(true);
    }

    // ---- 2. 绑定到上游 State：get/set 透传、target 指向源 ----
    {
        State<int> s{5};
        Binding<int> bd{s};
        AURORA_TEST_CHECK(bd.bound());
        AURORA_TEST_CHECK_EQ(bd.get(), 5);
        AURORA_TEST_CHECK(bd.target() == &s);
        AURORA_TEST_CHECK_FALSE(bd.removable());

        bd.set(9);
        AURORA_TEST_CHECK_EQ(s.get(), 9);  // 写回上游
        AURORA_TEST_CHECK_EQ(bd.get(), 9);

        bd.remove();  // 无回调 → 空操作，值不变
        AURORA_TEST_CHECK_EQ(s.get(), 9);
    }

    // ---- 3. 注入删除回调：removable 为真，remove 触发一次回调 ----
    {
        State<int> s{1};
        int removed = 0;
        Binding<int> bd{s, [&removed]() -> void { ++removed; }};
        AURORA_TEST_CHECK(bd.removable());
        AURORA_TEST_CHECK(bd.bound());
        AURORA_TEST_CHECK_EQ(bd.get(), 1);

        bd.remove();
        AURORA_TEST_CHECK_EQ(removed, 1);
        bd.remove();  // 可重复调用，回调按注入次数累加
        AURORA_TEST_CHECK_EQ(removed, 2);
    }

    // ---- 4. 非平凡值类型：get() 返回上游引用别名 ----
    {
        State<std::string> s{"hello"};
        Binding<std::string> bd{s};
        AURORA_TEST_CHECK_EQ(bd.get(), std::string("hello"));
        bd.set(std::string("world"));
        AURORA_TEST_CHECK_EQ(s.get(), std::string("world"));
        AURORA_TEST_CHECK(&bd.get() == &s.get());  // get 直接转发上游内部存储
    }
}

}  // namespace aurora::test_cases::utest_binding
