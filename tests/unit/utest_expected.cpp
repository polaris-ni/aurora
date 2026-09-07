/// 测试类型: unit
/// 目标单元: include/aurora/core/expected.h
/// 测试说明: 最小化 expected 二态包装（值/错误构造、拷贝移动、value/error/value_or）与 unexpected 单元测试

#include <string>
#include <type_traits>
#include <utility>

#include "aurora/core/expected.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_expected {

AURORA_TEST() {
    // ---- 1. unexpected 持有错误值，CTAD 推导错误类型 ----
    {
        const unexpected u{std::string("boom")};
        static_assert(std::is_same_v<std::decay_t<decltype(u.error())>, std::string>);
        AURORA_TEST_CHECK(u.error() == "boom");
    }

    // ---- 2. 值态构造：has_value / operator bool ----
    {
        const expected<int, std::string> e{42};
        AURORA_TEST_CHECK(e.has_value());
        AURORA_TEST_CHECK(static_cast<bool>(e));
        AURORA_TEST_CHECK(e.value() == 42);
    }

    // ---- 3. 错误态构造：unexpected 传入 ----
    {
        const expected<int, std::string> e{unexpected<std::string>{"nope"}};
        AURORA_TEST_CHECK(!e.has_value());
        AURORA_TEST_CHECK(!static_cast<bool>(e));
        AURORA_TEST_CHECK(e.error() == "nope");
    }

    // ---- 4. value_or：值态取值、错误态取默认 ----
    {
        const expected<int, std::string> ok{7};
        AURORA_TEST_CHECK(ok.value_or(0) == 7);

        const expected<int, std::string> bad{unexpected<std::string>{"err"}};
        AURORA_TEST_CHECK(bad.value_or(-1) == -1);
    }

    // ---- 5. 拷贝构造：值态与错误态各自保留 ----
    {
        const expected<int, std::string> a{5};
        const expected<int, std::string> b = a;
        AURORA_TEST_CHECK(b.has_value());
        AURORA_TEST_CHECK(b.value() == 5);

        const expected<int, std::string> c{unexpected<std::string>{"e"}};
        const expected<int, std::string> d = c;
        AURORA_TEST_CHECK(!d.has_value());
        AURORA_TEST_CHECK(d.error() == "e");
    }

    // ---- 6. 移动构造：值被搬走而非复制 ----
    {
        expected<std::string, int> a{std::string("payload")};
        const expected<std::string, int> b = std::move(a);
        AURORA_TEST_CHECK(b.has_value());
        AURORA_TEST_CHECK(b.value() == "payload");
    }

    // ---- 7. 拷贝赋值：同态直接赋值 ----
    {
        expected<int, std::string> a{1};
        const expected<int, std::string> b{2};
        a = b;
        AURORA_TEST_CHECK(a.value() == 2);

        expected<int, std::string> c{unexpected<std::string>{"x"}};
        const expected<int, std::string> d{unexpected<std::string>{"y"}};
        c = d;
        AURORA_TEST_CHECK(c.error() == "y");
    }

    // ---- 8. 跨态赋值：值态 -> 错误态 ----
    {
        expected<int, std::string> a{1};
        const expected<int, std::string> bad{unexpected<std::string>{"switch"}};
        a = bad;
        AURORA_TEST_CHECK(!a.has_value());
        AURORA_TEST_CHECK(a.error() == "switch");
    }

    // ---- 9. 跨态赋值：错误态 -> 值态 ----
    {
        expected<int, std::string> a{unexpected<std::string>{"switch"}};
        const expected<int, std::string> ok{99};
        a = ok;
        AURORA_TEST_CHECK(a.has_value());
        AURORA_TEST_CHECK(a.value() == 99);
    }

    // ---- 10. 移动赋值：跨态切换 ----
    {
        expected<std::string, int> a{std::string("first")};
        expected<std::string, int> b{unexpected<int>{404}};
        a = std::move(b);
        AURORA_TEST_CHECK(!a.has_value());
        AURORA_TEST_CHECK(a.error() == 404);
    }

    // ---- 11. 非平凡类型的析构路径（字符串值态被正确销毁，无泄漏由 ASan 兜底） ----
    {
        expected<std::string, std::string> e{std::string("value-side")};
        AURORA_TEST_CHECK(e.value() == "value-side");
        e = expected<std::string, std::string>{unexpected<std::string>{"error-side"}};
        AURORA_TEST_CHECK(e.error() == "error-side");
    }
}

}  // namespace aurora::test_cases::utest_expected
