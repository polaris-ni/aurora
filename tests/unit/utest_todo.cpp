/// 测试类型: unit
/// 目标单元: include/aurora/todo.h
/// 测试说明: 占位回调 TODO（任意 std::function 签名隐式转换、调用仅告警不崩溃、void 与非 void 返回默认值）单元测试

#include <functional>
#include <string>

#include "aurora/todo.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_todo {

AURORA_TEST() {
    // ---- 1. 持有描述文本 ----
    {
        const TODO t{"稍后实现保存逻辑"};
        AURORA_TEST_CHECK_EQ(t.what, std::string("稍后实现保存逻辑"));
    }

    // ---- 2. 隐式转换为 void 回调：调用只记警告、不崩溃 ----
    {
        const std::function<void(int)> cb = TODO{"wire click"};
        AURORA_TEST_CHECK(static_cast<bool>(cb));
        cb(42);  // 触发 Diagnostics::warn，无返回值
        AURORA_TEST_CHECK(true);
    }

    // ---- 3. 隐式转换为非 void 回调：返回结果类型的默认值 ----
    {
        const std::function<int(std::string)> f = TODO{"stub"};
        AURORA_TEST_CHECK_EQ(f("in"), 0);

        const std::function<double()> d = TODO{"num"};
        AURORA_TEST_CHECK_NEAR(d(), 0.0, 1e-12);
    }

    // ---- 4. 转换出的回调可拷贝保存并在稍后调用（生命周期自洽：desc 被捕获拷贝） ----
    {
        std::function<void()> stored;
        {
            const TODO t{"deferred"};
            stored = t;  // 拷贝转换：desc 捕获副本，t 析构后仍安全
        }
        stored();
        AURORA_TEST_CHECK(static_cast<bool>(stored));
    }
}

}  // namespace aurora::test_cases::utest_todo
