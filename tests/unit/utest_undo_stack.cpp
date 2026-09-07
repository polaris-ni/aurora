/// 测试类型: unit
/// 目标单元: include/aurora/state/undo_stack.h
/// 测试说明: UndoStack 撤销/重做/宏命令单元测试

#include <utility>
#include <vector>

#include "aurora/state/undo_stack.h"
#include "aurora_test_harness.h"

using au::UndoCommand;
using au::UndoStack;

namespace aurora::test_cases::utest_undo_stack {

AURORA_TEST() {
    // ---- 1. push 执行 redo，undo/redo 往返 ----
    {
        UndoStack stack;
        int value = 0;
        AURORA_TEST_CHECK(!stack.can_undo());
        AURORA_TEST_CHECK(!stack.can_redo());

        stack.push(
            {.redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "set 1"});
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.can_undo());

        stack.push(
            {.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "set 2"});
        AURORA_TEST_CHECK(value == 2);

        AURORA_TEST_CHECK(stack.undo());
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.can_redo());

        AURORA_TEST_CHECK(stack.undo());
        AURORA_TEST_CHECK(value == 0);
        AURORA_TEST_CHECK(!stack.can_undo());
        AURORA_TEST_CHECK(!stack.undo());  // 到底无操作

        AURORA_TEST_CHECK(stack.redo());
        AURORA_TEST_CHECK(value == 1);
        AURORA_TEST_CHECK(stack.redo());
        AURORA_TEST_CHECK(value == 2);
        AURORA_TEST_CHECK(!stack.redo());
    }

    // ---- 2. push 截断重做分支 ----
    {
        UndoStack stack;
        int value = 0;
        stack.push({.redo = [&]() -> void { value = 1; }, .undo = [&]() -> void { value = 0; }, .description = "a"});
        stack.push({.redo = [&]() -> void { value = 2; }, .undo = [&]() -> void { value = 1; }, .description = "b"});
        stack.undo();  // 回到 1
        stack.push({.redo = [&]() -> void { value = 9; },
                    .undo = [&]() -> void { value = 1; },
                    .description = "c"});  // 分支截断
        AURORA_TEST_CHECK(value == 9);
        AURORA_TEST_CHECK(!stack.can_redo());  // b 被丢弃
        AURORA_TEST_CHECK(stack.count() == 2);
    }

    // ---- 3. 描述与深度上限 ----
    {
        UndoStack stack;
        stack.set_limit(2);
        int v = 0;
        stack.push({.redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "one"});
        stack.push({.redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "two"});
        stack.push(
            {.redo = [&]() -> void { ++v; }, .undo = [&]() -> void { --v; }, .description = "three"});  // 挤掉 one
        AURORA_TEST_CHECK(stack.count() == 2);
        AURORA_TEST_CHECK(stack.undo_description() == "three");
        stack.undo();
        AURORA_TEST_CHECK(stack.redo_description() == "three");
        AURORA_TEST_CHECK(stack.undo_description() == "two");
    }

    // ---- 4. 宏命令整组执行/逆序撤销 ----
    {
        UndoStack stack;
        std::vector<int> order;
        std::vector<UndoCommand> cmds;
        cmds.push_back({.redo = [&]() -> void { order.push_back(1); },
                        .undo = [&]() -> void { order.push_back(-1); },
                        .description = "s1"});
        cmds.push_back({.redo = [&]() -> void { order.push_back(2); },
                        .undo = [&]() -> void { order.push_back(-2); },
                        .description = "s2"});

        stack.push(UndoStack::macro(std::move(cmds), "combo"));
        AURORA_TEST_CHECK(order.size() == 2 && order.at(0) == 1 && order.at(1) == 2);

        stack.undo();  // 逆序：-2 先于 -1
        AURORA_TEST_CHECK(order.size() == 4 && order.at(2) == -2 && order.at(3) == -1);
        AURORA_TEST_CHECK(stack.redo_description() == "combo");
    }
}

}  // namespace aurora::test_cases::utest_undo_stack
