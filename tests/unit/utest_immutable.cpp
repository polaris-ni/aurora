/// 测试类型: unit
/// 目标单元: include/aurora/core/immutable.h
/// 测试说明: 状态作用域权限包装 Immutable（只读）与 Mutable（读写）的透传语义与 scope 标签单元测试

#include <string>

#include "aurora/core/immutable.h"
#include "aurora/state/state.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_immutable {

AURORA_TEST() {
    // ---- 1. Immutable 透传读取底层 State 的当前值 ----
    {
        State<int> src{42};
        const Immutable<int> ro{src};
        AURORA_TEST_CHECK(ro.get() == 42);
    }

    // ---- 2. Immutable 观察到源状态的后续变化（持有引用，非快照） ----
    {
        State<int> src{1};
        const Immutable<int> ro{src};
        src.set(2);
        AURORA_TEST_CHECK(ro.get() == 2);
        src.set(3);
        AURORA_TEST_CHECK(ro.get() == 3);
    }

    // ---- 3. Immutable 的 scope 标签可标注读来源 ----
    {
        State<int> src{0};
        const Immutable<int> ro{src, "detail-pane"};
        AURORA_TEST_CHECK(ro.scope() == "detail-pane");
    }

    // ---- 4. Immutable 默认 scope 为空串 ----
    {
        State<int> src{0};
        const Immutable<int> ro{src};
        AURORA_TEST_CHECK(ro.scope().empty());
    }

    // ---- 5. Mutable 可写，写入落到源 State ----
    {
        State<int> src{0};
        Mutable<int> rw{src};
        rw.set(7);
        AURORA_TEST_CHECK(rw.get() == 7);
        AURORA_TEST_CHECK(src.get() == 7);
    }

    // ---- 6. Mutable 的写入可被同源的 Immutable 观察到 ----
    {
        State<int> src{0};
        Mutable<int> rw{src, "editor"};
        const Immutable<int> ro{src, "preview"};
        rw.set(99);
        AURORA_TEST_CHECK(ro.get() == 99);
        AURORA_TEST_CHECK(rw.scope() == "editor");
        AURORA_TEST_CHECK(ro.scope() == "preview");
    }

    // ---- 7. 同一 State 可挂多个权限包装，互不干扰 ----
    {
        State<std::string> src{"a"};
        Mutable<std::string> w1{src, "w1"};
        const Immutable<std::string> r1{src, "r1"};
        const Immutable<std::string> r2{src, "r2"};

        w1.set("b");
        AURORA_TEST_CHECK(r1.get() == "b");
        AURORA_TEST_CHECK(r2.get() == "b");
        AURORA_TEST_CHECK(r1.scope() != r2.scope());
    }

    // ---- 8. 包装非平凡类型（字符串）时的读取正确性 ----
    {
        State<std::string> src{};
        Mutable<std::string> rw{src};
        rw.set("hello");
        AURORA_TEST_CHECK(rw.get() == "hello");
        rw.set("world");
        AURORA_TEST_CHECK(rw.get() == "world");
    }
}

}  // namespace aurora::test_cases::utest_immutable
