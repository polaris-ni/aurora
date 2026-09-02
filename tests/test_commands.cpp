// test_commands.cpp — 命令式逃生舱（commands::run_raw）1:1 测试。
// 覆盖：回调执行、null 安全、闭包副作用、嵌套调用、与 State 协作等。
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

namespace commands = aurora::commands;
using aurora::State;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_commands ===\n");

    // 1) 基础执行：回调被调用。
    bool ran = false;
    commands::run_raw([&]() -> void { ran = true; });
    AURORA_TEST_CHECK(ran);

    // 2) null 回调安全（不崩溃）。
    commands::run_raw(nullptr);
    AURORA_TEST_CHECK(true);

    // 3) 修改外部计数器（副作用）。
    int counter = 0;
    commands::run_raw([&]() -> void { counter = 7; });
    AURORA_TEST_CHECK(counter == 7);

    // 4) 捕获并修改多个外部变量。
    int a = 1;
    int b = 2;
    commands::run_raw([&]() -> void {
        a = 10;
        b = 20;
    });
    AURORA_TEST_CHECK(a == 10 && b == 20);

    // 5) 嵌套调用：回调内再调用 run_raw。
    int depth = 0;
    commands::run_raw([&]() -> void {
        depth = 1;
        commands::run_raw([&]() -> void { depth = 2; });
    });
    AURORA_TEST_CHECK(depth == 2);

    // 6) 多次调用累积副作用。
    int total = 0;
    for (int i = 0; i < 5; ++i) {
        commands::run_raw([&]() -> void { total += i; });
    }
    AURORA_TEST_CHECK(total == 0 + 1 + 2 + 3 + 4);

    // 7) 传入显式 std::function 对象。
    const std::function fn = [&]() -> void { total += 100; };
    commands::run_raw(fn);
    AURORA_TEST_CHECK(total == (0 + 1 + 2 + 3 + 4 + 100));

    // 8) 回调读取外部状态并写入另一个外部状态。
    const std::string src = "hello";
    std::string dst;
    commands::run_raw([&]() -> void { dst = src; });
    AURORA_TEST_CHECK(dst == "hello");

    // 9) 与响应式 State 协作：回调内写入 State。
    auto s = std::make_shared<State<int>>(0);
    commands::run_raw([s]() -> void { s->set(42); });
    AURORA_TEST_CHECK(s->get() == 42);

    // 10) 无捕获 lambda 也执行。
    bool nocap = false;
    commands::run_raw([]() -> void {}); // 空回调
    commands::run_raw([&nocap]() -> void { nocap = true; });
    AURORA_TEST_CHECK(nocap);

    // 11) 回调中修改容器（push_back）。
    std::vector<int> v;
    commands::run_raw([&]() -> void {
        v.push_back(3);
        v.push_back(5);
    });
    AURORA_TEST_CHECK(v.size() == 2 && v[0] == 3 && v[1] == 5);

    // 12) 引用捕获：run_raw 返回后外部可见改变。
    std::string log;
    commands::run_raw([&log]() -> void { log += 'A'; });
    commands::run_raw([&log]() -> void { log += 'B'; });
    AURORA_TEST_CHECK(log == "AB");
}
